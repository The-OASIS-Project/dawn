# Upgrading DAWN

Notes for anyone updating an existing DAWN install to a newer version.

**Newest entries on top.** An entry is here only when an upgrade asks something of
you: a step to take, a setting to change or remove, a default you may rely on that
changed, or something that will look wrong if you don't know to expect it (a long
first start, a one-time rescan). Fixes and new features aren't listed: they need
nothing from you. A version with no entry upgraded cleanly.

This is not a changelog: git history and the pull requests cover everything else.

---

## 2026-10-22 — Per-turn context injection is on by default

**What changed.** `[memory.focus_injection] enabled` now defaults to true: each turn
retrieves the memories, documents and calendar events relevant to it and adds them to the
turn, marking documents and calendar events as outside text. This includes voice devices (satellites and the local microphone), so a reply spoken in
a shared room may mention what it retrieved, such as health or family details.

**What you need to do.** If others share a room with a voice device, turn it off in
Settings → Memory System → Per-Turn Context Injection (or set `enabled = false`). If you have ever
saved settings from the WebUI, your `dawn.toml` already says `enabled = false` under
`[memory.focus_injection]` and keeps it off; turn it on in the same place to get the new
default.

---

## 2026-10-22 — The built-in persona is new

**What changed.** The built-in persona is new and longer (values, then tone). A custom
`[persona]` or a replace-mode personal persona is unaffected; an append-mode one now adds to the
new text.

**What you need to do.** If a personal persona appends to the built-in one (My Settings →
Persona, append mode), read it again on top of the new text and trim anything that now repeats
or contradicts it.

---

## 2026-10-22 — Fact categories retired: `category_threshold` and `recategorize-all` are gone

**What changed.** DAWN no longer sorts memory facts into categories (personal, professional,
health, ...). Nothing used them: search and ranking ignored them and the Memory panel never showed them. The setting
that tuned the automatic sorting, `[memory.embeddings] category_threshold`, is ignored, and the
`dawn-admin memory recategorize-all` command is removed.

**What you need to do.** If your `dawn.toml` sets `category_threshold`, delete the line: DAWN
logs at startup that it is retired and ignored (a settings save from the WebUI also drops it).
Remove any script that runs `dawn-admin memory recategorize-all`.

---

## 2026-10-20 — Repeating reminders, timers and alarms that stopped may come back

**What changed.** A reminder, timer or alarm that rang for someone other than the local
speaker's user (anyone using DAWN only from the WebUI) never finished ringing, so a repeating
one stopped after its first ring and stayed "ringing" in the scheduler panel. DAWN now ends any
ring left open when it starts, and a repeating one goes on to its next time. On the first start
after this upgrade, those stopped series resume.

**What you need to do.** After the first start, open the scheduler panel and cancel any
repeating reminder, timer or alarm you don't want back, including ones you had re-created by
hand (they now fire twice).

---

## 2026-10-16 — libical is required; "UTC" in My Settings now means the server's zone

**What changed.** DAWN now reads clock times in a named time zone through libical, so building
from source needs `libical-dev` even with the calendar turned off. (The installer, the Docker
images and CI already include it.)

The time zone in My Settings used to default to "UTC" for everyone who never chose one. Those
settings are now "Same as server", the zone in Settings → Localization (`[localization] timezone`).
The time zone lists offer every zone, and two names that weren't real zones are corrected
(`America/Honolulu` → `Pacific/Honolulu`, `Asia/Mumbai` → `Asia/Kolkata`).

**What you need to do.**

- Building from source: `sudo apt install libical-dev` if you don't have it.
- If you deliberately chose UTC in My Settings, choose it again.
- If DAWN logs `Timezone '...' isn't a zone name DAWN can use` or `doesn't match this system's
  clock settings` at startup, set `[localization] timezone` to your zone (for example
  `America/New_York`).

---

## 2026-10-14 — Shutdown passphrase ignores case and punctuation

**What changed.** The `[shutdown]` passphrase is now compared ignoring case (for ASCII letters),
punctuation and spacing, so "Alpha-Bravo" and "alpha bravo" are the same passphrase. This applies to
voice, MQTT and the model alike. While shutdown is enabled with a passphrase, "shutdown
<passphrase>" spoken to the local microphone is handled on this device and never written into the
conversation or sent to the model, and "shut down" followed by other words is answered as a wrong
passphrase. That holds only for the local microphone in the `direct_first` and `direct_only`
command modes: in `llm_only`, and through WebUI voice or a satellite, what you say goes to the
model, passphrase included.

**What you need to do.** Nothing, unless your passphrase relies on case or punctuation to be
distinct; then pick one that differs in its letters or digits. A passphrase with no letters or
digits now always refuses; set a new one.

---

## 2026-10-14 — Settings saves no longer drop email, shutdown, plan or sound settings

**What changed.** A settings save from the WebUI rewrites `dawn.toml`, and it was deleting sections
it didn't write back. Fixed:

- `[email] enabled = true` was deleted, so the mail panel disappeared after the next restart. That
  key is retired: the mail panel is there whenever email is built in, and the model uses email only
  where `[llm.tools]` `local_enabled` / `remote_enabled` lists `"email"`, as before.
- `[shutdown]` (`enabled`, `passphrase`) was deleted, so voice shutdown turned itself off after a
  save. It's kept now, and changes made in the settings panel take effect without a restart. The
  passphrase is no longer shown to WebUI users who aren't admins.
- `[plan_executor] timeout_seconds` and `[sfx] sound_path` were deleted; they're kept now.

`[phone] enabled` and `[document_manage] enabled` are retired too. They never controlled whether the
model could use those tools; `[llm.tools]` did. DAWN notes each retired key once at startup and
ignores it, and the next settings save drops it.

**What you need to do.**

- If you set `[shutdown]`, `[plan_executor]` or `[sfx]` values and have saved settings since, check
  they are still in `dawn.toml`.
- To keep the model away from email, phone or document writes, make sure they aren't listed in
  `[llm.tools]` `local_enabled` / `remote_enabled`. The lists don't stop the mail panel or a
  scheduled email digest; to stop those, remove the email account or the briefing.

---

## 2026-10-13 — Memory: settings for removed search experiments are retired

**What changed.** Memory search experiments that lost in testing were removed, along with the
settings that switched them on. The behavior you had at the defaults is now the only behavior. These
`dawn.toml` keys are retired:

- `[memory.embeddings]` `rrf_enabled`, `bm25_enabled`, `memory_text_minimal`
- `[memory.graph_retrieval]` `entity_grounding_bonus`, `use_query_scoring`, `entity_bonus`
- `[memory.focus_injection]` `classifier_enabled` (it never did anything)

If your `dawn.toml` sets one, DAWN logs once at startup that it is retired and ignored, and the next
settings save from the WebUI removes it from the file. Their controls are gone from the settings
panel.

Also: memory search now shows what it knows about a person's or thing's relations (where someone
works, where they lived) when the assistant asks about a past date or for ones that have ended. Before,
those requests were accepted but returned no relations.

**What you need to do.** Nothing. If you had set any of these keys to a non-default value, your
memory search now runs on the defaults.

---

## 2026-10-09 — Claude Haiku 5.5 is the default Claude and OpenRouter model

**What changed.** When you use Claude without naming a model, DAWN now picks Claude Haiku 5.5
(`claude-haiku-5-5`) instead of Haiku 4.5. In DAWN's chat quality tests it was more accurate than Haiku
4.5 on text replies at about an eighth of the cost, and as accurate as OpenAI's `gpt-5.6-luna` while
starting to answer about 3 seconds sooner. OpenRouter's default model is now Haiku 5.5 too
(`anthropic/claude-haiku-5.5`, first in its built-in list, ahead of the OpenAI and Gemini models). The
default provider is unchanged (OpenAI, `gpt-5.6-luna`).

**What you need to do.** Nothing. If you saved your own Claude or OpenRouter model list (Settings →
Language Model, or `claude_models` / `openrouter_models` in `dawn.toml`), DAWN keeps it and its
default; to use Haiku 5.5 there, pick it in the model switcher or put it first. Conversations that
already have a model keep it.

---

## 2026-10-08 — Replies sized to the question; text messages kept short

**What changed.**
- Friday's base rules now say how long a reply should be (a sentence or two for
  a quick request, longer for advice or explanations) and to ask one short
  question when a request is missing something, instead of guessing.
- Replies by SMS are short plain text, without markdown or links.
- A satellite tells Friday the room it's in as a sentence, and that its Home
  Assistant area is the one to use when you don't name a room.
- On models that can't take a system message mid-conversation (among them
  Claude Haiku 4.5 and Sonnet 5, and most local models), DAWN's notes about the
  surface now come before your words in the message, so your question is the
  last thing the model reads. A short first message such as "turn it up" no
  longer gets an answer about offline hardware.
- A persona set to be "used instead of the base" (My Settings → Persona) is now
  the only persona Friday gets. Before, the base persona was sent too, with an
  instruction to ignore it.
- Each existing conversation misses the prompt cache once, on its next turn,
  then caches as before.

**What you need to do.**
- If your persona replaces the base one and you want Friday to keep something
  the base persona (`[persona] description` in `dawn.toml`) told her, such as
  the OASIS project list, add it to your own persona.
- If you set your own spoken-reply directions (`[tts] voice_directive` or
  `voice_directive_webui`): the rule about saying factorials in words ("52
  factorial", not "52!") moved from the base prompt into the built-in
  spoken-reply directions, so add it to yours if you want it.

---

## 2026-10-08 — Memory extraction: Claude Haiku 5.5 with reasoning is now recommended

**What changed.**
- A new setting, `[memory] extraction_effort` ("Extraction Reasoning" in
  Settings → Memory System), lets the model that pulls facts out of your
  conversations think before it writes them: `"off"`, `"low"`, `"medium"` or
  `"high"`. It defaults to `"off"`, which is how extraction has always run.
- The recommended memory setup is now Claude Haiku 5.5 with `"medium"`
  reasoning, replacing Claude Haiku 4.5. In DAWN's memory benchmark it
  remembers better (LoCoMo answer accuracy 73% → 77%) at about a fifth of
  Haiku 4.5's cost. The recommended compaction model moves to Haiku 5.5 too.
- Your settings don't change on their own: DAWN keeps the extraction and
  compaction models you already have.

**What you need to do.** Optional, but recommended if you use Claude for
memory extraction:
1. In Settings → Memory System, set **Extraction Model** to
   `claude-haiku-5-5` and **Extraction Reasoning** to `medium`.
2. If you compact with a dedicated model, set **Compaction Model** (Settings →
   Language Model) to `claude-haiku-5-5`.

Or in `dawn.toml`:
```toml
[memory]
extraction_model = "claude-haiku-5-5"
extraction_effort = "medium"

[llm]
compact_model = "claude-haiku-5-5"
```
Extraction runs in the background after a conversation, so reasoning doesn't
slow your replies. If your extraction runs on a local model on the same server
as DAWN, leave Extraction Reasoning off unless you've tested it: longer
extraction keeps that server busy.

---

## 2026-10-07 — Claude Haiku 5.5 added; default model shortlists refreshed

**What changed.** Claude Haiku 5.5 (`claude-haiku-5-5`, on OpenRouter
`anthropic/claude-haiku-5.5`) is supported: a 1M-token context, reasoning that
can be turned off, and about a tenth of Haiku 4.5's price. The built-in Claude
and OpenRouter shortlists in the model switcher now list Haiku 4.5, Haiku 5.5,
Sonnet 5.5 and Opus 5.5 in place of Sonnet 5, Opus 4.8 and Opus 5. The default
Claude model is still Haiku 4.5.

**What you need to do.**
- Nothing, if you've saved your settings from the WebUI before: your own model
  lists are kept. To add Haiku 5.5, add it in Settings → Language Model.
- If you keep your own `models.toml` (in `./`, `~/.config/dawn/` or
  `/etc/dawn/`), merge in the new `claude-haiku-5-5` rows from the shipped
  file: the `[anthropic]` context window, the `[thinking.anthropic]` row, and
  the `[mid_system]` list (which also gains `claude-sonnet-5-5`). Without them
  Haiku 5.5 still works, but with a 200K window, with reasoning that can't be
  turned off (memory extraction and summaries then pay for it), and without
  mid-conversation instructions.

---

## 2026-10-07 — Old conversation copies in `logs/` are safe to delete

**What changed.** Versions from December 2025 until 2026-09-30 saved a full copy
of a conversation to DAWN's `logs/` directory each time it was compacted (a long
conversation summarized to fit the model), in files named
`chat_history_session<N>_precompact_<date>_<time>.json`. Current versions don't
write them, but nothing removed the ones already there. They hold whole
conversations, and forgetting a memory or deleting a conversation doesn't reach
them.

**What you need to do.** Delete them unless you want to keep them. They're in the
`logs/` directory under the directory DAWN runs from (`/var/lib/dawn/logs/` for
the systemd service):

```bash
rm /var/lib/dawn/logs/chat_history_*_precompact_*.json
```

DAWN doesn't read them; your conversations are in the database.

---

## 2026-10-06 — Device data relayed over MQTT is read, not acted on

**What changed.** When a device answers over MQTT and DAWN speaks its data to
you (a camera description, a system status, anything sent back as device
data), the reply to it now only reads: Friday can look things up in your own
data and change the conversation's settings, but she can't act or prepare
an action (send, call, schedule, play, save), fetch from the web, or work a
device in that reply.
Device data can carry text from outside (a sign in a camera image, a web
page), so it no longer gets to start actions. Before, that reply had the full
tool set.

**What you need to do.** Nothing, unless a device relay of yours relied on
Friday acting on its data (for example, turning on a light because a camera
reported someone at the door). Ask for the action yourself, or set it up as a
Home Assistant automation. Actions you ask for by voice are unaffected.

---

## 2026-10-06 — Retired settings: delete the lines

**What changed.** These settings are ignored now. If your `dawn.toml` still sets
one, DAWN logs a note once at startup and carries on, and the next save from the
WebUI settings panel drops it.
- **`[llm] conversation_logging`** (the "Conversation File Logging" checkbox). It
  had stopped writing anything: conversations are kept in the database, where the
  WebUI and `dawn-admin` read them.
- **`recent_window_turns` and `score_uplift_factor`** under
  `[memory.focus_injection.dedup]`. A conversation now sends each retrieved memory,
  document passage or calendar event once and names it again on later turns
  instead of re-sending it, so the "don't re-send for N turns" rule they tuned is
  gone, along with its controls in the panel.
- **`[vision] capture_history_count`.** Camera captures are now kept with their
  conversation and bounded by the model (see 2026-10-01).

**What you need to do.** Nothing. Delete those lines to silence the startup note.

---

## 2026-10-05 — Scheduled unlocks and garage/gate opens no longer run

**What changed.** Unlocking, and opening anything that opens a door (a garage-door or gate cover, or a
switch, scene, script or automation whose name says garage, gate, door or unlock), now shows a preview
and waits for your "yes". So an unlock can no longer be scheduled, and a scheduled open of a garage door
or gate is refused when it fires. Scheduled blinds and awnings still open.

Calls, texts and emails to a contact matched only by part of a name, by a sound-alike, or by a name
DAWN read rather than heard from you now always come as a preview, even with
`[phone] confirm_outbound = false`.

**What you need to do.** If you had scheduled an unlock or a garage/gate open, ask for it when you want
it, or schedule it as a Home Assistant automation instead.

---

## 2026-10-03 — Actions by text need a reply code; background work reads but doesn't act; confirms need your reply

**What changed.**
- **A call, text or delete Friday previews is carried out only on your reply
  to the preview,** in the same conversation (device, chat or login) — the way
  email send already worked. Saying "yes" later, after talking about something
  else, or from another device no longer confirms it: ask again. If Friday
  prepares a second call (or a second text, or deletion) before you answer,
  only the newest one can be confirmed, and she prepares only one of each per
  message. A call, text or delete preview now lasts 5 minutes, as email's does.
- **Deep research starts the same way.** Friday proposes the run with its plan
  and cost; it starts only when your next message says yes.
- **Background work can't make calls, send texts or delete.** A background job,
  a follow-up after a job finishes, a scheduled briefing step, or an MQTT
  message asking for one is refused; Friday tells you what it would have done
  instead. This holds even with `[phone] confirm_outbound = false`.
- **Background jobs read and look things up, but don't act.** A job asked to
  send email, save a note, remember something, schedule, play music or
  render a chart refuses and reports what it would do; ask for it yourself.
  A job's follow-up in your conversation can't reach the web either.
- **A request by text message (SMS) acts only after you reply with a code.**
  Anyone can put your number on a text. Friday still answers questions and
  prepares things from a text, but sending, calling, deleting, searching the
  web or playing music now waits: DAWN texts your number what was asked and a
  6-digit code; reply with the code within 5 minutes to go ahead, or STOP to
  cancel (both are read that way only while a code waits). For something
  Friday previews first (an email, a call, a text), the code is your
  confirmation: no separate "yes" by text. Plans of several actions can't be approved by text; ask for them one
  at a time.
- **An action a tool doesn't have is refused** rather than passed on to it;
  Friday is told the tool's actions and tries again.

- **The phone sends up to 10 texts a minute** (was 5), and confirmation
  codes no longer count against that limit or the daily one.

**What you need to do.** Nothing. If your MQTT broker runs on another
machine, turn on TLS (`[mqtt] tls = true`): the codes cross it. DAWN now warns
at startup when it doesn't.

---

## 2026-10-02 — Messaging channels answer only the person who linked them; SMS links need a code

**What changed.**
- **A linked chat answers only the person who sent `/link`.** Before, a linked
  Telegram group answered everyone in it as you. Now other members are ignored,
  and each of them can link their own DAWN account from the same group. Posts
  by anonymous group admins, posts in a channel's or group's name, bots and
  forwarded messages are ignored too.
- **Several DAWN users in one chat no longer share a conversation.** Each
  linked account gets its own.
- **Linking a new SMS number needs one more step.** After you text
  `/link CODE`, DAWN texts back a 6-digit code; enter it in the WebUI
  (Settings → Messaging Channels). Anyone can put any number on a text, so
  this shows the number is really yours.
- **Codes made for one app only link that app**, if you picked an app when
  making the code. You can hold up to 5 unused codes at a time.
- **Unlinking an SMS number drops its verification**: linking it again needs a
  new code texted to it.
- **A text that is only a greeting ("Hi Friday") now gets an answer.** Before,
  it was dropped as having nothing to act on.
- **SMS verification relies on your MQTT broker.** If it accepts anyone, a
  `/link` text can be forged and the code read back; run it with
  authentication and ACLs on `echo/#`.

**What you need to do.**
- **Telegram groups you linked before this update stop answering until you
  re-link them**: send `/link CODE` in the group again. Your private Telegram
  chats, Discord and Slack keep working with nothing to do; numbers already
  linked over SMS keep working too.
- **If two DAWN users had linked the same chat**, the one used less recently
  is switched off (the daemon log names it). Each person re-links from their
  own account.
- **Remember that the assistant's replies in a group are seen by everyone in
  it.**

---

## 2026-10-02 — Email reading uses GMime: install `libgmime-3.0-dev` before rebuilding

**What you need to do.** If you build DAWN from source with email enabled (the default), install
the GMime library before you rebuild:

```bash
sudo apt install libgmime-3.0-dev
```

Without it, CMake stops with `gmime-3.0 not found`. If you don't use email, build with
`-DDAWN_ENABLE_EMAIL_TOOL=OFF` instead. The Docker images already include it.

**What changed.**
- **Both mail backends now read messages through one MIME reader.**
  - Mail in other character sets (Latin-1, Windows smart quotes, Japanese, and others)
    reads correctly.
  - HTML-only mail is read as text.
  - A message's attachments are listed by name, type and size, not just counted.
  - Cc and Reply-To are shown.
  - Senders' names, subjects and file names are cleaned of invisible and direction-reversing
    characters before the assistant sees them.
- **When a read fails, the assistant says why**: the server refused the login, access was
  revoked, the server was unreachable or slow, or the provider asked it to slow down.
- **Asking to trash an email no longer downloads it** just to show its sender and subject.
  On IMAP it also no longer marks the message read.
- **Gmail reads never download attachment bytes** to read the text.
- **IMAP trash and archive move only that one message,** into the folder your
  server marks for it (or one named Trash, Deleted Items, Archive). Before, the
  move erased every message marked deleted in that folder, including ones another
  mail app had only marked. **An account with no Trash (or Archive) folder now
  refuses that action** and leaves the message where it is; create the folder in
  your mail app if you want DAWN to use it.

---

## 2026-10-01 — Image messages attach images by id only (custom WebSocket clients)

**What changed.** A `text` message's images now come only from its `image_ids`
(the ids `POST /api/images` returns). The daemon reads the stored files and sends
the model exactly what a reload of the conversation sends. The old base64
`images[]` field is no longer read: a message that still carries it is handled as
if it didn't. If an id is malformed, names no image of yours, or there are too
many, the message is refused with an `error` frame (`IMAGE_UNAVAILABLE`,
`IMAGE_LIMIT` or `IMAGE_ERROR`) and nothing is added to the conversation; before,
DAWN sent what it could.

**What you need to do.** Nothing for the bundled WebUI (reload the page once so it
picks up the new script). A client of your own that sends images over the
WebSocket must upload each image first and send its id in `image_ids`; see
`docs/WEBSOCKET_PROTOCOL.md`.

---

## 2026-10-01 — Camera captures stay in the conversation

**What changed.**
- **An image a tool returns (a camera capture from `viewing`, an MCP image) is now
  kept with its conversation** in DAWN's image store, inside the tool's result, and
  reloads with the conversation. Before, a capture lived only in memory and older
  ones were blanked out as new ones came in. Captures are private to their owner and
  are deleted with their conversation. Guests' captures stay in memory only.
- **Images are bounded by the model**, replacing `[vision] capture_history_count`
  (retired, see 2026-10-06): a request carries at most
  the images its vendor allows (`models.toml` `[max_request_images]`), a capture past
  that is refused for that turn, and the next turn compacts the conversation so its
  oldest images are summarized away.
- **Captures have their own per-user limit (1000)**, separate from uploads, so a
  camera never fills the room your uploads need. At the limit a new capture is
  refused (the reply says so); deleting conversations that hold captures frees room.
- **A model without vision** (vision turned off for it) reads a short fixed note in
  place of each captured image.
- **Deleting a conversation deletes the images that belong only to it**: its
  captures, and the photos attached to its questions. This covers conversations
  from before this version too: the upgrade goes through your stored messages once
  and records which images each conversation's questions attached. An image another
  conversation also uses is kept. A reply, a memory or a note elsewhere that still
  mentions a deleted image can't show it any more: the model reads
  "[image no longer available]" in its place.
- **This upgrade rebuilds the message table once**, and needs free disk space next to
  the database for it (about twice the table's size, plus 64 MB). Without that room
  DAWN stops at startup with an error saying how much to free; nothing is changed.

**What you need to do.** Make sure the disk holding the database has that room
before you upgrade (or free it and start DAWN again). If you keep a customized
`models.toml`, merge in
the new `[max_request_images]` table from the shipped one to tune the limits; without
it DAWN uses the built-in copy.

---

## 2026-09-30 — Logging out signs out every tab, at once

**What changed.**
- **Logout is immediate and complete.** Logging out used to take up to 30 seconds,
  and could leave the session behind until DAWN was restarted; enough logouts in a
  row ended in "Maximum sessions reached". Now the reply comes at once, every tab on
  that login is signed out and closed, and its sessions are freed.
- **An ended login signs its tabs out everywhere,** whatever ended it: a logout in
  another tab, logging in as someone else in the same browser, revoking the login
  (WebUI or `dawn-admin`), changing the account's password, deleting the user, or
  the login expiring. Before, those tabs kept working until they were reloaded.
- **Changing your own password signs you out everywhere else.** Your other tabs,
  Aurora and other devices go to the login screen; the tab you changed it in stays
  signed in. An admin resetting someone's password (WebUI or `dawn-admin`) signs that
  user out everywhere.
- **Everyone signs in once more after this update.** The login cookie is renamed
  (`dawn_session` becomes `__Host-dawn_session`), so the old one is no longer read.
  The `__Host-` prefix tells the browser to accept that cookie only from DAWN itself,
  over HTTPS, for the whole site: a cookie planted by another site on your domain can
  no longer stand in for your login.
- **Aurora and the WebUI have separate logins.** With both open in one browser,
  logging out of (or into) one no longer affects the other. Aurora has its own login
  now (`__Host-dawn_session_aurora`).
- **Scripts that log in directly** (curl, Python) must read and send the new cookie
  name, `__Host-dawn_session`.
- **A browser's music stream needs its login cookie.** The WebUI and Aurora send it
  already; a custom client that opens the music socket without the cookie now gets
  `auth_failed`. Satellites are unaffected.

**What you need to do.** Sign in again (in the WebUI, and in Aurora if you use
it). If you run a separate front-end behind a proxy, make sure the proxy passes the
`Cookie` header to the music port and does not add a `Domain` to DAWN's cookies (the
browser rejects a `__Host-` cookie that has one).

---

## 2026-09-30 — The database is rewritten once, then keeps itself compact

**What changed.**
- **A one-time rewrite at the first start.** DAWN converts `auth.db` to
  incremental auto-vacuum with a single `VACUUM` when it starts. Measured on a
  Jetson: about 0.8 seconds for 71 MB of data; larger databases take longer, and
  startup waits for it. The rewrite is all-or-nothing: if it stops part way, the
  file is as it was.
- **It needs free disk space for the rewrite:** about the size of your data next
  to the database (twice that when the database is over 512 MB, half of it in
  your temp directory). Without the room, DAWN skips the conversion, logs
  `not converting to incremental auto-vacuum yet`, and tries again at the next
  start. Everything else works as before meanwhile.
- **From then on the file shrinks by itself.** Space freed by deleted
  conversations, users and stored tool results is returned to the disk within
  about a minute, instead of the file keeping its largest size forever.
- **Shorter pauses on large writes.** Writing the database's log back into the
  file now happens on its own thread, not while every other part of DAWN waits.
  Measured on a Jetson, a 16 MB write now holds the database ~90 ms, from
  ~130-180 ms.

- **Large tool results are now stored too** (a long web page, a big MCP
  response), whole, with their conversation, and deleted with it: up to 256 MB
  per conversation, 1 GB per user and 4 GB in all, oldest first.

**What you need to do.** Nothing, as long as the disk has the space above. A
manual `VACUUM` (the admin compact command) briefly needs twice the database's
size on disk, as before.

**Note on deleted data.** Deleting a conversation removes it from the database
file promptly (its space is returned to the disk). Like any deleted file, its old
contents can remain in freed disk blocks, in the database's log until it is
overwritten, and in any backup copies (DAWN's pre-upgrade backups in
`backups/`, and any copies you made yourself). Tool results are kept at rest in
`auth.db` the way the conversation's messages are; back it up and protect it the
same way. Full-disk encryption is the way to protect data at rest on the device.

---

## 2026-09-29 — Existing conversations migrate once

**What changed.** A conversation now keeps the prompt it started with and adds
changes where they happen (edited instructions, the surface you're on, each
question's time and retrieved memory), each reply is saved with the model's own
reasoning so it replays exactly, and a long conversation is summarized between
turns, never mid-reply. Cloud providers can then reuse their prompt cache across
the whole conversation, which costs less and answers faster. On Claude Opus 5.5
and Fable 5.1 accounts created on or after 2026-08-31, it also stops the "system
prompt re-rendered" error on the turn after a tool call. The database migrates
itself on first start (schema v92 to v96).

**What you'll notice.**
- **The first message in each existing conversation fixes its prompt.** The
  model's earlier reasoning there isn't replayed from then on (its text and tool
  calls are). The daemon log says `prefix boundary (adopted)` once per such
  conversation. A conversation that was already compacted loses its earlier
  reasoning once the same way.
- **A long conversation is summarized between turns.** The summary appears as a
  "CONVERSATION SUMMARY" block in front of the first kept message. A reply that
  fills the conversation mid-task answers with what it has (or says it ran out of
  room); send another message to carry on.
- **A tool that isn't available where you are stays listed,** and DAWN refuses it
  if called. A newly connected MCP server's tools take effect with a
  conversation's next turn.
- **Old voice conversations are rewritten once.** Before this version, saving a
  voice conversation lost part of every turn that used a tool. On the first start
  DAWN turns those messages into proper tool entries where it can; where a call was
  never saved, the turn becomes a note ("[Tool Call: its name and arguments weren't
  saved]") followed by its results. The daemon log says how many rows were
  rewritten.

**Worth knowing.**
- **Saved reasoning is for the model only.** It's never shown in the WebUI,
  exported, searched or sent to another model, and only the provider that
  produced it gets it back.
- **The memory and context sent with each question are stored with the
  conversation** (never shown, searched, exported or extracted). Forgetting a
  memory item, deleting memories, or deleting a document also removes it from
  that stored context, including in open conversations. What the assistant said
  about it (its replies, tool results it read) stays; delete the conversation to
  remove those too.
- **Backups made before the upgrade still hold the old voice messages,**
  reasoning included. Delete or keep them as you would any backup with private
  content.

**What you need to do.** Nothing.

---

## 2026-09-28 — Reasoning is set per model; the WebUI shows only what a model takes

**What changed.**
- **Each model gets the reasoning settings it actually supports**, from a new
  `[thinking.*]` table in `models.toml`. DAWN now always sends a reasoning
  setting the model accepts. Models that can't turn thinking off (Claude Opus
  5.5, Sonnet 5.5, Fable 5) get adaptive thinking at their lowest effort when
  you've asked for none; before, the setting was left out and they thought at
  medium. Background calls (summaries, memory extraction) use each model's
  cheapest setting.
- **Reasoning is no longer switched off after a tool call,** and every provider
  uses the effort you set for the conversation. A tool-heavy turn on a
  thinking model can therefore use more output tokens than before; lower the
  effort if cost matters.
- **The WebUI reasoning menu lists only the modes and effort levels the current
  model takes**, works mid-conversation, and keeps your pick across reloads and
  model switches (a model that can't honor it gets the nearest setting, and
  a notice says so).
- **Direct Claude requests degrade instead of failing when an earlier turn's
  reasoning can't be replayed.** On Claude accounts created on or after
  2026-08-31, that used to be an error; now the reasoning is dropped and the
  request goes through. The database migrates itself (schema v91).
- **An OpenAI or Gemini model `models.toml` doesn't list gets no reasoning
  setting** (DAWN used to guess from the model's name). That includes a newer
  `gpt-5.x` than the ones listed.

**What you need to do.** Nothing if you use the shipped `models.toml`. If you
keep your own copy, it doesn't need the new table: DAWN uses the built-in one
for any table your copy lacks. To give an unlisted model reasoning controls,
add a row to the `[thinking.openai]`, `[thinking.anthropic]` or
`[thinking.gemini]` table, following the shipped examples.

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
  were dropped every time a reply was prepared. Now they're included with the
  phone owner's requests for 10 minutes after the event, on every surface, and
  they're no longer
  stored in conversations. Other users see only the incoming-call banner; while
  the *Local Device* has no user assigned, the phone belongs to the *Default
  Voice User*. Proactive alerts (Watches) do the same when *Make
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
