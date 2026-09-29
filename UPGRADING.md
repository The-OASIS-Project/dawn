# Upgrading DAWN

Notes for anyone updating an existing DAWN install to a newer version.

**Newest entries on top.** Each entry is dated by its implementation date and is
included **only when an upgrade needs your attention** — a changed default, a
manual migration step, or a behavior change that affects you. A version with no
entry here upgraded cleanly with nothing for you to do.

This is not a full changelog (see git history for that) — it is the short list of
"things a human upgrading should read," in plain language.

---

## 2026-09-29 — Merging duplicate memories is recoverable again

**What changed.**
- **Merges are real merges.** When the assistant merged duplicate memories
  (`forget` with a fact to keep), the fact to keep never reached DAWN, so every
  such "merge" permanently deleted the duplicates. Duplicates are now hidden
  behind the kept fact and recoverable, as intended.
- **A merged fact is kept for `prune_superseded_days` (default 30) from the
  merge.** Before, the window counted from when the fact was first learned, so
  any fact older than that was deleted at the next prune, right after merging.

**Upgrading.** Nothing to do: the database migrates itself (schema v95), and
facts merged before the upgrade get a fresh window from the upgrade. Duplicates
deleted by earlier "merges" are not brought back; the facts they were merged
into still hold what they said.

---

## 2026-09-29 — Conversations keep the prompt they started with

**What changed.**
- **A conversation's system prompt is fixed when it starts.** Before, DAWN
  rebuilt it on every turn, so any change (the time, your memory, a device
  coming online) re-sent the whole conversation to the model as new. Now a
  change reaches the conversation where it happened, and nothing already sent
  is rewritten:
  - Editing your persona or settings mid-conversation adds a note to the
    conversation that the instructions changed, with the new text. The model
    follows the newest.
  - The surface you're talking through (voice, a messaging channel, a
    satellite's room) and which tools are unavailable right now are added as
    standing directions when they change.
  - The current time, the memory items retrieved for a question, and new
    device events (a phone ringing) go in front of the question they belong to.
  - What DAWN knows about you (your preferences and recent conversations) goes
    in front of a question when it changed since the conversation last had it.

  Cloud providers can then reuse their prompt cache across the whole
  conversation, which costs less and answers faster. On Claude Opus 5.5 and
  Fable 5.1 accounts created on or after 2026-08-31, it also stops the
  "system prompt re-rendered" error on the turn after a tool call.
- **A conversation offers the same tools on every turn.** A tool that isn't
  available where you are right now (a device offline, a tool disabled for
  this kind of session) stays listed; the model is told it's unavailable, and
  DAWN refuses it if called. A newly connected tool (an MCP server) takes
  effect with the conversation's next turn, as a fresh start for the model's
  earlier reasoning (text and tool calls are kept).
- **Memory citations keep their number.** A memory item shown as `[M7]` is
  `[M7]` for the whole conversation, across reloads.
- **Image turns send the image on every request of the turn**, the same way a
  reloaded conversation does.

**Upgrading.** Nothing to do: the database migrates itself (schema v94). The
first message you send in each existing conversation fixes its prompt; the
model's earlier reasoning in that conversation isn't replayed from then on (its
text and tool calls are). The daemon log says `prefix boundary (adopted)` once
per such conversation.

**Worth knowing.**
- The memory and context sent with each question are stored with the
  conversation, like the question. They never appear in the WebUI, search,
  exports or memory extraction. Forgetting a memory item, deleting memories
  in the memory panel, or deleting a document also removes it from the
  context stored in your conversations,
  including ones open right now; the model's earlier reasoning in those
  conversations isn't replayed after that, since it may have repeated the item.
  What the assistant itself said about it (its replies, tool results it read)
  stays in the conversation; delete the conversation to remove those too.
- Phone notices (an incoming or missed call, an arriving text) now go only to
  the conversations of the user who owns the phone (the local device's voice
  user), not to every connected user. With no user assigned to the local
  device, no conversation gets them; the incoming-call banner still shows.
- The debug chat logs (`logs/chat_history_*.json`) now include that memory and
  context, as they were sent.

---

## 2026-09-29 — Reloaded conversations replay as the model produced them; old voice conversations cleaned up

**What changed.**
- **Each reply is now saved with its model's reasoning, for the model only.** When
  a conversation is reopened, resumed as a background job, or continued from a
  messaging channel, each earlier reply goes back to the model exactly as that
  model produced it, reasoning included, instead of as plain text. Replies keep
  their quality, and cloud providers can reuse their prompt cache. The reasoning
  sits in a new database column that is never shown in the WebUI, exported,
  searched or sent to any other model. Only the provider that produced it gets
  it back (and, where a provider ties it to an account, only through the same
  API key). It is removed once a conversation is compacted past it. A database
  copied to another DAWN install can't replay it there.
- **Old voice conversations are cleaned up automatically.** Before this
  version, saving a voice conversation lost part of every turn that used a
  tool:
  - With Claude, the model's raw reply structure (tool calls, results, and any
    reasoning) was stored as the message text, so reasoning reached memory
    extraction and a reloaded conversation gave the model raw blocks instead of
    real tool calls.
  - With other models, the tool calls were dropped: the turn was saved as an
    empty assistant message (an empty box in the WebUI) and its results were
    saved without the call they answer, so the WebUI didn't show them.

  On the first start after upgrading, DAWN rewrites those messages: calls and
  results become proper tool entries where they can be paired, reasoning is
  dropped, and where the call itself was never saved the turn becomes a note
  ("[Tool Call: its name and arguments weren't saved]") followed by its results
  ("[Tool Result: …]"), shown as tool entries in debug mode. The daemon log
  says how many rows were rewritten. Messages DAWN saves from now on keep
  their tool calls.

**What you need to do.** Nothing. The database migrates itself on first start
(schema versions 92 and 93).

**Worth knowing.**
- Backups made before the upgrade still hold the old voice messages, reasoning
  included. Delete or keep them as you would any backup with private content.
- Facts memory already extracted from those old voice conversations stay as
  they are.
- Rolling back to an older DAWN version is safe: older versions ignore the new
  column.

---

## 2026-09-27 — Memory: private-conversation fix, forget what a conversation taught, imported memories now searchable; unassigned satellites are guests

**What changed.**
- **A satellite not assigned to a user is now a guest.** Before, anyone speaking to
  an unassigned satellite was treated as the *Default Voice User* (Settings → Memory,
  usually the admin): they could ask about that user's memories, calendar, email,
  documents, reminders, stock portfolio, text messages and background jobs, and the
  conversation was saved to that user's history and learned from. Now an unassigned
  satellite gets no personal data, persona or alerts: DAWN answers general questions
  and controls the house, and says the device isn't assigned when asked for something
  personal. Its conversations aren't saved or learned from. A ringing alarm can still
  be snoozed or dismissed from it. The microphone connected to the DAWN machine now belongs to
  whoever its speaker does: the *Local Device* on the satellite management page. While
  that is unassigned it speaks for the *Default Voice User*. Tools called without any session
  (for example over MQTT) also act for the Default Voice User; before, some of them
  used the first account instead.
- **Privacy fix: private conversations no longer reach memory when a session ends.**
  When a WebUI, satellite or messaging session ended (idle timeout, a messaging
  channel's `/new` reset, or daemon shutdown), DAWN summarized the conversation it
  had open into long-term memory without checking whether that conversation was
  marked private. Opening a private chat just to read it was enough. Switching
  conversations or starting a new one already respected the private flag; the
  session-end path didn't. It does now, and it also no longer summarizes a
  background job's transcript or a conversation you deleted. The same session-end
  path used to summarize a conversation a second time later on; that's fixed too.
- **Switching conversations mid-reply no longer mixes them.** If you opened
  another conversation while DAWN was still answering (or while a message was
  queued), the reply could land in the conversation you switched to and become
  part of its context for later answers. Each reply now stays in its own
  conversation, and a message is always answered with its own conversation's
  context. The same applies to voice: a spoken message is saved to the
  conversation it was said in. Continuing a private conversation (after it was
  compacted) now keeps it private, and a finished background job's report is no
  longer remembered as something you said.
- **Imported memories are now searchable by meaning.** Memories added with
  Memory → Import were stored without embeddings, so DAWN could only find them by
  exact keywords and never brought them up on its own. New imports are embedded in
  the background within seconds and sorted into categories. On first start after
  upgrading, DAWN also embeds any existing facts that are missing embeddings, for
  every user, and if the embedding service is briefly unreachable it now retries a
  few minutes later instead of waiting for the next restart.
- **Fact categories are re-run.** The background pass that sorts imported and older
  facts out of "general" into categories (personal, professional, interests, ...) had
  been saving nothing. The upgrade (database schema v88) runs it again for every
  user. It only touches facts still marked "general". Facts learned from
  conversations already get their category when they're extracted.

- **Marking a conversation private can now also forget what was learned from it.**
  When you switch an existing conversation to private and DAWN has already learned
  something from it, you're asked whether to forget those memories too. Only what
  that conversation taught is forgotten, including anything saved there with
  "remember that ...". A fact you also mentioned in another conversation stays, and
  so do memories you imported (Memory → Import). If a forgotten fact had replaced an
  older one (say, a new address), the older one is forgotten with it rather than
  coming back as current, unless it was imported; the prompt counts those
  separately as older versions. Making a conversation private now
  also makes every conversation that continues it private, since each continuation
  starts with a summary of the one before.
- **Switching the model in a conversation now sticks to it.** When you ask DAWN to
  change its model, the change is saved to that conversation (like the model
  setting in the conversation's menu), so it still applies when you come back to it.
  Before, a spoken switch in the WebUI lasted only until you opened another
  conversation. A switch that comes from something DAWN read rather than from you
  (a background job's result, a web page) lasts only for that one reply. Such a
  switch can never move a conversation from the local model to a cloud one. Nor is a
  switch you ask for saved when it would move a private conversation to the cloud.
  In both cases DAWN says so.
- **Deleting a conversation still keeps what DAWN learned from it; forgetting is the
  new way to remove it.** That's how delete always worked. What changes is how it
  works alongside forget: a memory another conversation also taught now points at that
  conversation, and one no other conversation taught counts as learned outside any
  conversation. Forgetting a different conversation that repeats it therefore leaves
  it alone. To remove what a conversation taught, forget it (mark it private) before
  deleting it.
- **DAWN now knows about phone calls when you speak to it.** Notices like "the phone
  is ringing" or "the call was answered elsewhere" were meant to reach the model but
  were dropped every time a reply was prepared. Now they're included with your
  requests for 10 minutes after the event, on every surface, and they're no longer
  stored in conversations. Proactive alerts (Watches) do the same when *Make
  Conversations Aware* is on (Settings → Proactive Attention; off by default), but
  only on the watch owner's own devices.
- **Voice fixes.**
  - Interrupting DAWN with the wake word while it is answering now answers what you
    said next. It used to cancel the reply and then ignore your new request, since
    the reply hadn't finished stopping. An interruption during a tool call also no
    longer leaves a half-finished tool call behind that made the next request fail.
  - Saying a stop phrase ("Okay Friday, stop", "never mind", or just "stop" over her
    speech) now stops the reply and she confirms right away ("Stopped, Sir."). Before,
    "Okay Friday, stop" was sent to the model for a reply, and a bare "stop" stopped
    her silently. A stopped request stays in the conversation, marked as stopped,
    so "do that again" still knows what you meant.
  - "Think hard" by voice now applies to that one request instead of staying on.
  - A satellite sends one query at a time: speaking again stops the query still
    running instead of both being answered at once.
  - A satellite can no longer register with the all-zero id
    `00000000-0000-0000-0000-000000000000`: it is reserved for the DAWN machine's own
    mic and speaker (the *Local Device*). A client using it could take over the Local
    Device's owner and room. If a client used it, give it its own id. The Local
    Device's name is restored at startup if it was changed this way.
  - Remapping a satellite to a different user in the WebUI saves the previous
    user's voice conversation first, so one user's speech never ends up in another
    user's conversation or memory.
- **Better memory recall on large histories.** Memory search compared your question
  against only the first 2,000 memories it held, and it held at most 8,192 per user,
  so beyond either limit the rest were never searched by meaning. It now searches
  every memory it holds, and holds as many as fit in 80 MB per user (new setting
  *Semantic Search Memory*, `[memory] fact_cache_mb`): about 50,000 with the default
  embedding model at about 1.6 KB each (13 MB for 8,000 memories), fewer with a larger
  model. A user with more keeps
  the memories used in the last 90 days and links to notes first, then the most
  confident and recent; the log says how many were left to keyword search. Expired
  memories are no longer held.
- **Fewer unrelated documents in DAWN's context.** DAWN used to add passages from
  your documents to nearly every answer, whether they were related or not, and only
  ever looked at the first ~128 passages it found. It now considers all of your
  documents and adds a passage only when it clearly relates to what you asked. The
  new *Document relevance floor* setting (Settings → Memory → Per-Turn Context Injection,
  `document_min_relevance`, default 0.48) controls how strict that is. Documents it
  doesn't add are still available when you ask DAWN to search them. The same idea
  applies, more gently, to your memories (*Memory relevance floor*,
  `fact_min_relevance`, default 0.34): a fact is added automatically only when it
  relates to what you said, so an arithmetic question no longer drags in your
  anniversary. Asking DAWN to search its memory is unaffected.
- **Memory search now understands accented and non-English text.** The built-in
  (ONNX) embedding model only understood plain ASCII: accented words ("café",
  "résumé"), other languages and even curly apostrophes were largely lost when
  memories and documents were indexed. It now reads text the way the model was
  trained to. Because this changes how text is indexed, **the first start after
  upgrading re-indexes your memories and documents once** in the background. It takes
  about 0.15 s per memory and document passage: a few minutes for a small memory,
  around half an hour for tens of thousands of entries. DAWN stays usable meanwhile.
  This only applies with the default built-in embedding provider; OpenAI or Ollama
  embeddings are unchanged.
- **Database schema v89** adds a record of which conversations each memory
  (fact, relationship, preference) was learned from, seeded from existing
  memories; indexes that make forgetting fast; and bookkeeping that lets DAWN keep
  document search in memory. It runs automatically on first start. Memories saved
  before this update only know the most recent conversation they came from, so
  forgetting an older conversation can't find them; check the Memory panel for
  those.

**What you need to do.**
- **If people use a satellite as themselves, assign it to them** on the satellite
  management page (admin). Until then it's a guest and can't reach their memories or
  data.
- **If you use private conversations (including private messaging channels),** look
  through Memory for anything that came from a private chat, and delete it. There's
  no reliable way to tell those entries apart automatically. Summaries are the most
  likely place to find them.
- Nothing else. The embedding, re-indexing and category work runs in the background
  after the first start (see the re-index note above for how long it takes).

---

## 2026-09-24 — Email: per-account digest depth, and older mail on IMAP accounts

**What changed.**
- Each email account has a new **Digest depth** setting (Settings → Email → edit
  account → Advanced Settings). It's the most messages the daily digest will look
  through in that inbox. The digest now pages back through an inbox until it covers
  the whole digest window, so a busy day is no longer cut off at 50 messages. Depth
  defaults to 50, the same as before, so nothing changes until you raise it (up to 200).
  Lower it for a busy inbox you don't need fully covered.
- The **Max recent emails** setting now actually takes effect. When you ask for recent
  mail without saying how many, that account returns this many (default 10, as
  before). Asking for a specific number works as it always did.
- **IMAP accounts** (non-Gmail) can now page back to older mail, the way Gmail
  accounts already could.
- **Fix for large IMAP mailboxes:** listing or searching a big IMAP inbox could
  quietly drop messages (commonly showing one fewer than asked for) because of a
  limit in the curl library. DAWN now searches in small batches, so results are
  complete.

**What (if anything) to do.** Nothing. The database upgrades itself on first start.
If a digest line says "digest depth N reached", raise that account's Digest depth.

---

## 2026-09-22 — Music library re-scans once to add genre + year (and fixes genre loss)

**What changed.** The music scanner now extracts **genre** and **release year** from
your local files' tags (FLAC/MP3/Ogg) and from Plex, and stores them in the music
database. This also **fixes a bug** where a local track's genre could be silently
cleared whenever its file changed, because the previous scanner never wrote genre back.
Searching for a genre ("play some jazz") now works against local files, not just Plex.

**What (if anything) to do.** Nothing — it's automatic, but be aware of a **one-time
slow scan**: on first startup after this upgrade, DAWN re-reads every local music file
once to backfill genre/year (existing entries are otherwise skipped by the
unchanged-file check). Expect that first scan to take about as long as your original
initial scan; subsequent startups are back to normal. No config change, and your Plex
tracks refresh on the normal sync.

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
