# Upgrading DAWN

Notes for anyone updating an existing DAWN install to a newer version.

**Newest entries on top.** Each entry is dated by its implementation date and is
included **only when an upgrade needs your attention** — a changed default, a
manual migration step, or a behavior change that affects you. A version with no
entry here upgraded cleanly with nothing for you to do.

This is not a full changelog (see git history for that) — it is the short list of
"things a human upgrading should read," in plain language.

---

## 2026-09-03 — llama-server: thinking control moved to `--reasoning`

**Local LLM users only.** If DAWN uses a cloud provider, nothing here applies.

**What changed.** llama.cpp renamed how "don't think, just answer" is requested.
DAWN's llama-server config now writes `REASONING_MODE=off` instead of the old
`LLAMA_CHAT_TEMPLATE_KWARGS='{"enable_thinking":false}'` line. Also adds Preset K
(Qwen 3.8 27B) and refreshes every preset's advertised speed/quality from a
single re-benchmark of the whole fleet.

**Does this affect me?**

- **🔴 If you are on llama.cpp b9360 (2026-05-27) or newer and have NOT re-run
  the installer — you are probably already broken, and it is silent.** In b9360
  llama.cpp renamed the environment variable that old line used
  (`LLAMA_CHAT_TEMPLATE_KWARGS` → `LLAMA_ARG_CHAT_TEMPLATE_KWARGS`). Your
  existing `/usr/local/etc/llama-cpp/llama-server.conf` still sets the old name,
  which nothing reads any more, so thinking is **on** — the model's whole reply
  goes into `reasoning_content` and DAWN receives an **empty message**. Nothing
  errors; replies just come back blank or truncated.

  Check it in one command (with llama-server running):
  ```bash
  curl -s -X POST http://127.0.0.1:8080/v1/chat/completions \
    -H "Content-Type: application/json" \
    -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":30}' \
    | head -c 400
  ```
  If `"content"` is empty and `"reasoning_content"` is full of text, you have it.

  **Fix — re-run the installer for your preset**, which regenerates the config
  with the new key and installs the updated service unit:
  ```bash
  sudo ./services/llama-server/install.sh -P J    # or your preset letter
  ```

- **Which llama.cpp do I need?** `REASONING_MODE` needs **b8287** (2026-03-11) or
  newer — that is when `--reasoning on|off|auto` was added. Practically any build
  from the last six months qualifies. Preset K (Qwen 3.8) additionally needs
  **b10419+** to load its GGUF, but that is a Preset-K-only requirement.

  Check yours with `llama-server --version`. On a build older than b8287,
  **do not re-run the installer** — the generated config would pass a flag your
  binary does not understand and llama-server would fail to start. Upgrade
  llama.cpp first.

- **Preset numbers changed.** Every preset was re-benchmarked on one build /
  one machine / one suite, so the advertised speed and quality figures moved —
  previously they had been collected across several llama.cpp versions and two
  hardware generations and were not comparable to each other. Notably Gemma 4
  31B now measures 97.4% (highest local score) and Gemma 4 26B-A4B's "blocked on
  thinking leak" note is resolved by `--reasoning off`. This is informational —
  no action needed.

- **Nothing else to do.** No database migration, no `dawn.toml` change. If you
  are on an older llama.cpp and do not re-run the installer, your setup keeps
  working exactly as before.

---

## 2026-09-02 — Memory citation reinforcement + transient-state extraction guard

**What changed.** DAWN can now reinforce a memory fact's confidence when the
assistant actually *cites* that fact in an answer — a small boost, capped and
limited to once per fact per hour — so genuinely-used memories persist and rank
higher over time. Separately, memory extraction now **rejects transient device and
system state** (battery %, on/off/locked status, live device counts, the current
time) as durable facts; that class was being stored and later recalled with stale
values instead of triggering a live tool call.

**Does this affect me?**

- **New installs — no action.** Reinforcement is **on by default**. A fresh
  database never accumulates stale device-state facts (the extraction guard is
  always on), so reinforcement is clean-safe from day one.

- **Upgrading — reinforcement is OFF by default (opt-in).** Your existing
  `dawn.toml` is untouched and behaves exactly as before. To turn it on:
  - In the WebUI: **Settings → Memory → "Citation Reinforcement Boost"** (set to
    `0.05`), with **"Memory Citation Signal"** enabled; or
  - In `dawn.toml`, under `[memory.decay]`: `citation_enabled = true` and
    `citation_reinforcement_boost = 0.05`.

- **Optional cleanup before enabling (upgraders only).** If your memory holds
  device-state facts from before this version, you can regenerate them cleanly:
  ```
  dawn-admin memory reextract --user <username> --confirm
  ```
  This drops derived memory and re-extracts your conversations under the new
  prompt (so the stale device-state facts don't come back), and supports a backup
  path. **It is optional** — reinforcing a leftover stale fact has a small, capped
  effect that self-heals as the fact stops being cited and decays away.

- **Automatic.** The database schema migrates itself on first launch of the new
  binary (it adds a `last_cited` column to `memory_facts`). No manual step.
