# Upgrading DAWN

Notes for anyone updating an existing DAWN install to a newer version.

**Newest entries on top.** Each entry is dated by its implementation date and is
included **only when an upgrade needs your attention** — a changed default, a
manual migration step, or a behavior change that affects you. A version with no
entry here upgraded cleanly with nothing for you to do.

This is not a full changelog (see git history for that) — it is the short list of
"things a human upgrading should read," in plain language.

---

## 2026-09-17 — `system_status` gains a `network` view (requires a STAT upgrade)

**What changed.** The `system_status` tool can now report network status — your
interfaces, primary uplink (lowest-metric default route), gateway reachability,
and the cellular link — via a new `network` action (also folded into `all`). Ask
things like "what's my network status?".

**What (if anything) to do.** This depends on new telemetry from the **STAT**
sensor service: DAWN only shows network data if STAT is publishing its `Network`
message type. **Upgrade STAT to a build that emits `Network` telemetry** (dated
2026-09-17 or later). Until you do, the `network` action simply reports "No
network telemetry from STAT" and the rest of `system_status` is unaffected —
nothing breaks, the new view is just empty. No DAWN config change is required
(same `[stat]` topic).

---

## 2026-09-17 — New optional "stocks" tool (Charles Schwab)

**What changed.** DAWN gains an opt-in **stocks** tool for live quotes and
read-only Schwab portfolio access. It's off unless you configure it, so this is
purely additive — nothing changes for existing installs that don't want it.

**What (if anything) to do.** Only if you want it:
1. Add `[secrets.schwab]` `client_id` / `client_secret` (and optionally
   `redirect_url`) to `secrets.toml`.
2. Run `dawn-admin schwab auth` on the host to link your account.

Note the Schwab refresh token expires every **7 days**, so you re-run
`dawn-admin schwab auth` about weekly. Full instructions:
[docs/SCHWAB_SETUP.md](docs/SCHWAB_SETUP.md). Trading is not included (read-only).

---

## 2026-09-16 — WebUI reconnect hardening + optional "Always on" per-browser mode

**What changed.**
- The WebUI now runs an app-level heartbeat and detects a *half-open* socket (one
  that looks "Connected" but the server has stopped answering) instead of sitting
  silently dead — a real cause of "it says connected but nothing saves." It
  reconnects indefinitely (no more giving up after a few tries), recovers on tab
  focus/network-online, and re-syncs the conversation list on reconnect. A briefly
  shaky link shows an amber "Unstable…" state.
- New per-browser opt-in **"Always on (this browser)"** in the user menu (beside
  "Alarm sounds"). When you enable it (after accepting a security prompt), that
  browser stays signed in for as long as it's open — its login session is renewed
  while it actively heartbeats, up to a **30-day** absolute cap — instead of
  expiring at 24h. **Enable it only on a device you personally trust:** anyone with
  access to that browser stays logged in as you until you turn it off or sign out.
- The auth database migrates automatically to **schema v85** (adds one column). No
  action needed; the daemon logs `migrated schema from v84 to v85` on first start.

**What you need to do.** Nothing required. Restart the daemon to pick up the schema
migration and the keepalive support; hard-refresh open WebUI tabs to load the new
client. "Always on" is off by default. If you don't enable it, login-expiry behavior
is unchanged (you'll simply see a non-navigating "signed out" state instead of an
abrupt redirect that could lose an in-progress message).

---

## 2026-09-13 — Email: sending now requires you to choose the account

**What changed.** The assistant's `email` `send` action now **requires** an explicit
`account` naming which of your configured accounts the message goes out *from*.
Previously, sends silently used the first writable account, so a reply could leave
from the wrong mailbox. The chosen account is now bound to the draft when it is
prepared and used unchanged when you confirm the send, and the confirmation readback
tells you which account it will send from.

**What you need to do.** Nothing for normal use — the assistant now names the sending
account and reads it back before sending, and is instructed to reply from the account
the original message was addressed to. **If you created a scheduled/automated task
that sends email**, make sure it specifies an `account`; a `send` with no account now
returns an error instead of guessing.

---

## 2026-09-11 — Optional: serve the Aurora UI from DAWN at /aurora

**New opt-in; nothing changes unless you turn it on.** If you don't run Aurora,
skip this — the default is off and your install is unaffected.

**What changed.** DAWN's WebUI server can now serve the Aurora front-end from its
own origin under `/aurora/`, sharing one login, cookie, and API/WebSocket with the
main WebUI — no separate port, host, or CORS setup. It is gated by a new `[webui]`
setting, `aurora_path`, which is empty (disabled) by default.

**What to do (only if you want Aurora).** Build Aurora (`npm run build`), then in
`dawn.toml`:

    [webui]
    aurora_path = "/absolute/path/to/aurora/dist"

Point it at the built `dist/` directory (not the repo root), using an absolute
path, and restart DAWN. When enabled, an Aurora link appears in the WebUI header
and `/aurora/` serves the app behind the same login. Leaving `aurora_path` empty
keeps the WebUI-only behavior. DAWN validates the path at startup and disables the
feature (with a log warning) if it does not exist or has no `index.html`.

---

## 2026-09-10 — IMAP accounts now show unread & replied state in digests

**Only affects non-Gmail (IMAP) email accounts.** Gmail accounts were already
covered; nothing to do either way.

**What changed.** Email digests and briefings now read the `\Seen` and `\Answered`
flags directly from IMAP servers, so IMAP messages get accurate **unread** markers and
**replied** indicators — previously these were populated for Gmail accounts only and an
IMAP inbox would report "0 unread." Message receive times also now come from the
server's INTERNALDATE (with correct timezone handling), so the digest's 24-hour window
is accurate for IMAP mail regardless of the sender's timezone. No configuration change;
the improvement is automatic the next time a digest runs.

---

## 2026-09-10 — Briefings can carry summarization instructions

**Optional, opt-in — nothing to do unless you want it.** Existing briefings are
unchanged and keep summarizing exactly as before.

**What changed.** A briefing can now carry a free-text **instructions** field that
steers *how* its collected data is summarized — what to emphasize, how to structure
it, length, and tone. For example: "Lead with anything time-sensitive, keep it under
five bullets, skip the pleasantries." A briefing with no instructions uses the same
default format it always has.

**How to use it.** Two ways: ask your assistant — "make my morning briefing punchier
and lead with unread email," or "drop the closing line from the market briefing" — or
edit it in the WebUI: open the **Scheduler** panel, expand a briefing, and use the
**Edit** button in its "Summarization instructions" section. Either way the
instructions **replace** the previous ones wholesale rather than appending (an empty
save clears them back to the default), so read them back first if you mean to amend.

**Schema.** The auth database migrates to v84 automatically on first launch (adds one
nullable column to `scheduled_events`). No manual step; the upgrade is transparent.

**Safety note.** Briefing instructions can shape the *format* but cannot override the
rule that the data a briefing collects is data, never instructions — a summarization
prompt can't be steered into treating an email's contents as commands.

---

## 2026-09-09 — Scheduled email steps are now read-only

**Only if you have a scheduled task/briefing that runs an email *send*, *trash*, or
*archive* step.** Nothing to do for anyone else.

**What changed.** A scheduled or briefing step that invokes the `email` tool is now
restricted to read-only actions (`accounts`, `recent`, `search`, `folders`, `read`,
`digest`). A step that tries to `send`, `trash`, or `archive` mail is
refused at both creation and fire time — those actions require a live conversation
(a human in the loop). This closes a gap where a scheduled email step could send or
delete mail unattended. If you had such a step, recreate it as an interactive flow;
read-only email briefings are unaffected.

---

## 2026-09-09 — `dawn-admin` binary now builds beside `dawn`

**Anyone rebuilding in an existing build directory.** Fresh checkouts and clean
builds are unaffected.

**What changed.** The `dawn-admin` CLI now builds to `<build>/dawn-admin`
(next to the `dawn` binary) instead of the nested `<build>/dawn-admin/dawn-admin`.
This matches the layout the getting-started docs already show. The install/test
scripts that hardcoded the old nested path were updated to the flat path in the
same change.

**Does this affect me?** Only if you have an **existing** build directory from
before this change. It still contains a `dawn-admin/` *directory*, which now
collides with the new same-named output *file* — your next build fails with:

```
/usr/bin/ld: cannot open output file dawn-admin: Is a directory
```

**Fix (one time).** Remove the stale directory, then rebuild:

```bash
rm -rf build-debug/dawn-admin        # or your build dir (build/, build-release/, …)
make -C build-debug -j8
```

A full clean rebuild (fresh build directory) also resolves it. After this,
invoke the CLI as `./build-debug/dawn-admin …` (no second `dawn-admin/`).

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
