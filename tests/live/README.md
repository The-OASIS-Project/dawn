# Live tests

End-to-end tests against a real DAWN daemon: a throwaway instance driven over
the WebUI's WebSocket protocol, with fake LLM providers. They test what a user
sees (the reply, the errors, what was saved) without touching your config,
database, network or accounts.

```bash
make -C build-debug dawn dawn-admin
tests/live/run.sh                        # every test_*.py here
tests/live/run.sh test_local_fallback    # one file
DAWN_LIVE_KEEP=1 tests/live/run.sh       # keep each instance's run dir
DAWN_LIVE_BUILD=/path/to/build tests/live/run.sh
```

Also `ctest --test-dir build-debug -L live`, which tests that build.

Needs: `unshare` (util-linux) with unprivileged user namespaces, `ip`
(iproute2), Python 3 with `requests` and `websocket-client`, and the model
files under `models/` (the tiny Whisper model, a Piper voice, Silero VAD, the
embedding model). Without the namespaces or the Python modules, `run.sh` exits
77 (skipped). Ubuntu 23.10 and later restrict unprivileged user namespaces
through AppArmor (`kernel.apparmor_restrict_unprivileged_userns`); there the
tests are skipped unless an AppArmor profile allows `unshare`.

## What is isolated

- **Network.** `run.sh` runs everything (fakes, daemon, client) in new user,
  network, mount and PID namespaces with only loopback. Nothing reaches the
  internet, the LAN (a local LLM, Home Assistant, Plex, MQTT, MCP servers) or
  your daemon's admin socket, whose abstract name is per namespace. Nothing
  outlives the run.
- **Real state on disk.** `/var/lib/dawn`, `/etc/dawn`, `/run` and
  `~/.config/dawn`, `~/.local/share/dawn` are covered by an empty tmpfs, so a
  default path the run dir doesn't override still can't reach your data. The
  harness refuses to start unless this and the network fence are in place.
- **Config and data.** Each instance gets a run dir under `/tmp` with its own
  `dawn.toml` (written from scratch, not from yours: no MQTT, OTA, memory,
  attention, research or code projects; localhost only, plain http), its own
  `secrets.toml` (only what the test passes), data dir, crypto key and home.
  `models`, `www`, `tool_instructions` and `sound_assets` are symlinked from the
  checkout. Your `dawn.toml`, `secrets.toml` and `auth.db` are never read.
- **Environment.** The daemon starts with `PATH`, `LD_LIBRARY_PATH`, `LANG` and
  `HOME` only: it reads API keys, bot tokens and `DAWN_*` settings from the
  environment otherwise.
- **Process.** `--server`: no microphone or speaker.

The run dir is deleted after a passing class and kept (and printed) after a
failure.

## Writing a test

- A test class is a `LiveTestCase`; its `setUpClass` calls
  `start_instance(config, secrets_toml, fakes)`, and everything is stopped when
  the class ends. `config` is `{section: {key: value}}` over the defaults in
  `instance.py`. The class's tests share the daemon and run in name order
  (`test_1_`, `test_2_` when one builds on another).
- `Client(port, user, password)` logs in as the instance's throwaway admin;
  `turn(text)` returns the reply, the error frames and the time taken.
- `FakeLLM(port, name)` is an OpenAI-compatible provider. `script(...)` queues
  responses: `text()`, `tool_call()`, `status()`; with an empty script it gives
  a default answer. Not starting it leaves its port closed. Count one turn's
  requests with `chat_requests(nonce)`, with a nonce in the turn's text (it
  matches the requests' last user message, so later turns don't count). The
  script is one queue for every request: give background models (extraction,
  compaction) a fake of their own when a test turns them on.
- Assert on what a user sees (frames, saved rows via `inst.db()` /
  `shown_roles()`), and on the fake's requests. Read the daemon's log
  (`log_mark()` / `log_since()`) only for what has no other observable.
