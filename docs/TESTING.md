# How DAWN Is Tested

What tests and checks DAWN has, what each one covers, what runs on every change, and what isn't covered.
Numbers are as of October 2026. The size and test counts reproduce with `scripts/code_metrics.sh`, the
coverage with the commands in [Coverage](#coverage).

## Contents

- [At a glance](#at-a-glance)
- [Unit tests](#unit-tests)
- [Build-time invariant checks](#build-time-invariant-checks)
- [Sanitizers](#sanitizers)
- [Static analysis](#static-analysis)
- [Fuzzing](#fuzzing)
- [Integration scripts](#integration-scripts)
- [Benchmarks](#benchmarks)
- [Coverage](#coverage)
- [What CI runs](#what-ci-runs)
- [Running it locally](#running-it-locally)
- [Not covered](#not-covered)

## At a glance

DAWN is about 224,000 lines of first-party C and C++ in the daemon, plus the WebUI, satellites, admin CLI
and tests (387,000 lines in all, comments and blank lines not counted; `scripts/code_metrics.sh` gives
the breakdown).

| Layer | What | Runs |
|---|---|---|
| Unit tests | 194 suites, 2,892 test functions, 12,105 assertions (Unity) | every commit (hook) and every push (CI) |
| Invariant checks | 9 build-time rules + 2 source checks | every build, every push |
| Sanitizers | the unit tests under ASan + UBSan, and under TSan | every push |
| Static analysis | clang-tidy (bugprone, cert, the clang static analyzer), CodeQL, `-Wall -Werror` at `-O2` | every push and pull request; CodeQL weekly too |
| Fuzzing | 5 libFuzzer harnesses over parsers that read outsiders' bytes | every push |
| Integration scripts | a running daemon: security, admin CLI, satellite protocol | by the developer, before a release |
| Benchmarks | memory retrieval, LLM quality and latency, ASR | by the developer |

## Unit tests

`tests/` holds one standalone program per suite, written with [Unity](https://github.com/ThrowTheSwitch/Unity).
Each links the sources it tests plus small stubs, so a suite runs in milliseconds to seconds and needs
no daemon, network or hardware. Suites that need a database use SQLite in memory or under a per-process
temporary path (`tests/test_tmp.h`), so suites run in parallel without sharing state.

Suites carry a ctest label:

- **`ci`** (194 in the `ci` preset): no hardware, no models, no network. These run everywhere.
- **`hardware`** (5): need a GPU, ONNX Runtime models or audio devices, and run on the developer's
  Jetson.

The `debug` preset registers one more `ci` suite than the `ci` preset: `test_code_project_git` needs
libgit2 1.6 or later, which Ubuntu 22.04 doesn't package.

## Build-time invariant checks

Some rules can't be expressed in C types, so scripts in `scripts/` enforce them. Each is a CMake target
the daemon depends on, so every build runs them and fails if one breaks. The nine rules are listed in
[ARCHITECTURE.md](../ARCHITECTURE.md#build-time-invariant-checks); among them, every WebSocket frame goes
through one send queue, every messaging send through one formatting funnel, and stored LLM turn blocks are
read only to rebuild a request.

Two more source checks run as `ci` tests: `check_settings_sections_rendered.sh` (every settings section the
WebUI defines is one it renders) and `check_swap_eligible_types_mirror.sh` (a list the C and JavaScript
sides both keep stays in sync).

## Sanitizers

The `ci` suites also build and run under two sanitizer presets, `asan` and `tsan`. Any report fails the
suite that triggered it.

- **`asan`**: AddressSanitizer with leak detection, and UndefinedBehaviorSanitizer with recovery off, so
  undefined behavior fails the run.
- **`tsan`**: ThreadSanitizer, for the units that start threads (the session reaper, the turn queue, the
  job pool, the database storage thread, the MCP transport and others).

## Static analysis

- **clang-tidy**, version pinned by hash (`.github/clang-tidy-requirements.txt`), over every source in
  `src/` and `common/src/`. Checks: `bugprone-*`, `cert-*` and the clang static analyzer
  (`clang-analyzer-*`). `.clang-tidy` makes every finding an error, and lists each check that is turned off
  with its reason. A false positive is suppressed where it occurs, with the reason beside it.
  `scripts/run_clang_tidy.py` runs it.
- **CodeQL** (GitHub code scanning) for C/C++, JavaScript and the workflow files, on pushes to `main`,
  on pull requests and weekly. Results are on the repository's Security tab.
- **Compiler warnings**: the Docker image builds the server preset at `-O2` with `-Wall -Werror`, under
  GCC 12. The optimizer enables warnings a debug build doesn't produce (`-Wmaybe-uninitialized`,
  `-Wstringop-truncation`, `-Wformat-truncation`, `-Warray-bounds`), so this build is where those are
  enforced.
- **Formatting**: clang-format for C and C++, Prettier for the WebUI, checked on every push
  (`./format_code.sh --check`).

## Fuzzing

`tests/fuzz/` has a [libFuzzer](https://llvm.org/docs/LibFuzzer.html) harness for each parser that reads
bytes an outsider controls. Each is built with ASan and UBSan.

| Harness | Reads | Where the bytes come from |
|---|---|---|
| `email_mime` | a raw RFC 822 message, an address header, a Gmail JSON payload | anyone who sends mail |
| `html` | HTML into Markdown (with and without a base URL) and into plain text | web pages, HTML mail |
| `neutralize` | text DAWN shows the model but didn't write (`llm_context_neutralize`) | tool results, web pages, messages |
| `tts_text` | a reply before it's spoken (number, date, currency, URL and Markdown handling) | whatever the model repeats |
| `caldav` | a calendar-query REPORT (XML with iCalendar inside) and a sync-collection page | the calendar server |

Each starts from tracked seeds in `tests/fuzz/corpus_<harness>/`, with a token dictionary for CalDAV.
CI runs each for 60 seconds on every push; a crash, leak, timeout or sanitizer report fails the job and
the input is uploaded. `tests/fuzz/run_fuzzers.sh [seconds] [harness...]` runs them locally.

## Integration scripts

These need a running daemon (and, for some, credentials, a registered satellite or a local service), so
CI doesn't run them. The developer runs them before a release.

| Script | Tests |
|---|---|
| `tests/test_pentest.sh` | authentication, authorization, XSS, path traversal, the satellite endpoint and security headers, against a live daemon |
| `tests/test_dawn_admin.sh` | the `dawn-admin` CLI over the admin socket |
| `tests/test_satellite_protocol.py` | the DAP2 satellite protocol: register, query, ping |
| `tests/smoke_test.sh` | each build configuration links and starts (the daemon links ONNX Runtime and Piper, which aren't apt packages) |
| `tests/smoke_test_harness.sh` | the coding harness: a project import from a local repository fixture through to "ready" |
| `tests/search_quality/test_search_quality.sh` | search result quality against a SearXNG instance, saved for before/after comparison |

## Benchmarks

Benchmarks measure quality, not correctness, and are run by hand.

- **Memory retrieval** (`benchmarks/`): LongMemEval, LoCoMo and ConvoMem. Several metrics are reported and
  only one is comparable to published systems; [benchmarks/README.md](../benchmarks/README.md) explains
  which, and a run labels itself `LEADER-COMPARABLE: YES|NO`.
- **LLM quality and latency** (`llm_testing/`): local and cloud models on DAWN's prompts and tools
  (`test_llm_quality.py`), and local inference speed (`test_llama_performance.sh`).
- **ASR** (`test_recordings/`): word error rate and speed on recorded commands
  ([BENCHMARK_RESULTS.md](../test_recordings/BENCHMARK_RESULTS.md)).

## Coverage

Unit-test line coverage, measured by the `coverage` preset and gcovr (`gcovr.cfg` counts DAWN's own code,
not the tests or vendored code).

**What the number means.** gcovr counts only files a test compiles. Unit tests compile 300 of DAWN's 526
C and C++ source files, about half of the code by lines (115,000 of 220,000). In those files, **61.7% of
lines** and 49.9% of branches run. Across all of DAWN's code, unit tests run roughly a third of it. The rest
(the WebUI server, `dawn.c`'s main loop, MQTT, ASR and TTS engines, the network clients) is exercised by
the integration scripts and by use, and isn't counted here.

| Directory | Source files a test compiles | Line coverage in those files |
|---|---:|---:|
| `src/core` | 64 of 81 | 79.2% |
| `src/tools` | 55 of 127 | 67.6% |
| `src/messaging` | 7 of 17 | 63.6% |
| `src/audio` | 6 of 25 | 64.4% |
| `src/auth` | 48 of 61 | 62.7% |
| `src/llm` | 51 of 51 | 50.2% |
| `src/memory` | 40 of 48 | 48.9% |
| `src/config` | 4 of 4 | 45.8% |
| `src/webui` | 6 of 52 | (the six are small helpers) |
| `common` (shared with the satellite) | 11 of 15 | 69.6% |
| `dawn_satellite` | 3 of 30 | 77.2% |
| `src/asr`, `src/tts`, `src/ui`, `dawn.c`, MQTT | 0 | — |

CI measures coverage on every push to `main` and puts the summary on the run's page, with the full HTML
report as an artifact.

## What CI runs

On every push and pull request (`.github/workflows/ci.yml`), in parallel:

| Job | What |
|---|---|
| `format-check` | clang-format and Prettier over the whole tree |
| `source-checks` | invariants that need no build |
| `unit-tests` | the `ci` preset: build the suites, run `ctest -L ci` |
| `sanitizers` | the same suites under `asan` and under `tsan` |
| `clang-tidy` | the pinned clang-tidy over every DAWN source; any finding fails |
| `fuzz` | each libFuzzer harness for 60 seconds |
| `docker-build` | the full image, `-Wall -Werror` at `-O2`; the daemon links and runs `--help` |
| `satellite-build` | the Raspberry Pi satellite, headless |
| `esp32-build` | the ESP32 satellite firmware |
| `coverage` | pushes to `main` only: instrumented suites and the gcovr report |

Also: CodeQL (`codeql.yml`) and the OpenSSF Scorecard (`scorecard.yml`). The `ci` preset builds without
the WebUI, ONNX Runtime, CUDA or the Whisper submodule, so it runs on a stock Ubuntu 22.04 runner; the
clang-tidy job turns the WebUI on so its sources are checked too.

Locally, the git hooks (`./install-git-hooks.sh`) check the formatting of what you stage, and when source
files are staged, build and run the `ci` suites before the commit; the pre-push hook runs them again.

## Running it locally

```bash
# Unit tests (plain `make` doesn't rebuild test binaries)
cmake --preset debug
make -C build-debug tests-ci
ctest --test-dir build-debug -L ci

# One suite
make -C build-debug test_iso8601 && ./build-debug/tests/test_iso8601

# Sanitizers
cmake --preset asan && make -C build-asan tests-ci && ctest --test-dir build-asan -L ci
cmake --preset tsan && make -C build-tsan tests-ci && ctest --test-dir build-tsan -L ci

# Coverage
cmake --preset coverage && make -C build-coverage tests-ci
ctest --test-dir build-coverage -L ci && gcovr build-coverage --print-summary

# clang-tidy (pinned version; any build directory's compile database works)
pip install --require-hashes -r .github/clang-tidy-requirements.txt
make -C build-debug models_toml_builtin
scripts/run_clang_tidy.py build-debug

# Fuzzing (clang with libFuzzer)
tests/fuzz/run_fuzzers.sh 60

# Size and test counts
scripts/code_metrics.sh --build-dir build-ci
```

On a GitHub-hosted Ubuntu 22.04 runner the sanitizers need `sudo sysctl -w vm.mmap_rnd_bits=28` first.

## Not covered

- **Integration tests aren't in CI.** The scripts above need a running daemon with credentials, a
  satellite or a local service; they are run by hand before a release.
- **Little of the WebUI server has unit tests.** Its WebSocket and HTTP handlers, the session and
  connection lifecycle, and audio streaming are exercised by the integration scripts and by use.
- **The browser code has no automated tests.** The WebUI's JavaScript is checked for formatting and by
  CodeQL, not by tests.
- **Hardware paths** (audio devices, GPU ASR, TTS) are tested on the developer's Jetson (`hardware`
  label), not in CI.
- **Code-projects** (the coding harness) needs libgit2 1.6 or later, so its git suite and its sources'
  clang-tidy pass run on the developer's machine, not in CI. The same goes for clang-tidy on the two files
  that include ONNX Runtime's headers (`memory_embed_onnx.c`, `text_to_speech.cpp`), which isn't packaged
  for Ubuntu.
