# Upgrading DAWN

Notes for anyone updating an existing DAWN install to a newer version.

**Newest entries on top.** Each entry is dated by its implementation date and is
included **only when an upgrade needs your attention** — a changed default, a
manual migration step, or a behavior change that affects you. A version with no
entry here upgraded cleanly with nothing for you to do.

This is not a full changelog (see git history for that) — it is the short list of
"things a human upgrading should read," in plain language.

---

## 2026-10-05 — Friday doesn't learn from text messages

**What changed.**
- **Your SMS conversation is private.** Anyone can put your number on a text, so
  nothing said by text is learned into memory. A new SMS link gets a private
  conversation, and an existing one becomes private when the next text arrives
  (and again if it's ever made public). You still see it in the WebUI, marked
  private; Friday still reads her memory there. "Remember …" by text still
  works, after the reply code. Being private, it's also not searched from your
  other conversations, and an LLM switch asked for by text lasts for that reply
  only. Telegram, Discord and Slack, which vouch for who sent each message, are
  unchanged.

**What you need to do.** Nothing. What earlier texts already taught stays in
memory; review it in the Memory panel if you want it gone.

---

## 2026-10-05 — Private mode applies from the first message

**What changed.**
- **Turning on private mode before you start a conversation now covers its first
  message.** In the WebUI, a conversation you mark private before typing (or
  speaking) is created private, so nothing from its first turn is learned.
  Before, it was made private only after the first turn had started, and a
  conversation started by voice ignored the setting.
- **Research from a private conversation stays out of memory.** Its report is
  still saved as a note you can read, without the memory entry that pointed to it.

**What you need to do.** Nothing.

---

## 2026-10-05 — Home Assistant and calendar ask instead of picking

**What changed.**
- **Two devices that match equally ask which.** "Turn on the lamp" with a desk
  lamp and a floor lamp now lists both instead of switching on whichever came
  first.
- **Unlocking and opening ask first.** Unlock, open, and anything that opens a
  door (a garage-door or gate cover, or a switch, scene, script or automation
  whose name says garage, gate, door or unlock) shows a preview and waits for
  your "yes". Lock, close and turning things off act right away. Blinds and
  awnings don't ask. Because they need your yes at the time, unlock and open
  can no longer be scheduled.
- **Those devices also need their name.** Friday acts on them only when what you
  said is the device's name or part of it ("front door", "garage"); a looser
  match, such as "garage side door" for "Garage Main Door", is offered back to
  you.
- **Adding an event to a named calendar never lands elsewhere.** A calendar name
  that matches nothing, or more than one writable calendar, gets the list of
  your writable calendars instead of going to the first one. Every new event
  says which calendar it went to.

**What you need to do.** Nothing.

---

## 2026-10-04 — Friday asks before calling, texting or emailing someone she isn't sure of

**What changed.**
- **A name that's only part of a contact's asks first.** "Call Chris" with a
  Christine in your contacts used to call Christine; now Friday shows who she
  found ("Christine Lee, for 'Chris'") and asks. The whole name, or a whole
  word of it ("call Lee"), goes through as before.
- **By voice, names that sound alike ask first.** With both a Cris and a Chris
  in your contacts, a spoken "call Chris" asks which one.
- **A recipient you didn't name asks first.** If Friday picks up a name or an
  address from something she read (an email, a web page) rather than from you,
  the call, text or email draft says so and waits for your yes.
- **"My wife", "mom" and other names not in your contacts** are looked up in
  Friday's memory of you, and the preview asks you to confirm the person she
  found.
- These always come as a preview you answer with "yes", even with
  `[phone] confirm_outbound = false`.
- **An email's "to" must be one address.** A list of addresses is refused
  rather than sent to all of them.
- If you set your own `[asr] disambiguation_hint`, it keeps your wording; the
  built-in one now also says to pass names as heard.

**What you need to do.** Nothing.

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

## 2026-10-02 — Sending and trashing email need your yes in a new message

**What changed.**
- **The assistant can no longer prepare an email and confirm it in the same
  reply.** It drafts, reads it back, and sends only if your very next message
  says yes. If the conversation moves on first, it prepares the email again.
  Trash works the same way. Voice is unchanged: it asks, you say yes.
- **A confirm counts only in the session where the draft was made**: the same
  browser tab, device or channel. A draft prepared in the WebUI can't be
  confirmed from a satellite, or from another tab.
- **Background jobs, re-engaged background turns, scheduled tasks and MQTT
  messages can't send, trash or archive email.** The assistant says what it
  would do and leaves it to you. A build without the WebUI (no multi-client
  support) can't send, trash or archive email at all, since it can't tell your
  voice from an MQTT message.
- **The read-back now ends with a fixed line**, "Sending to <address>, from
  <account>, subject: ...". The assistant is asked to say it as written, so you
  hear where the mail really goes.

**What you need to do.** Nothing.

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

---

## 2026-10-02 — Email trash and archive over IMAP no longer purge other deleted mail

**What changed.**
- **Trashing or archiving a message on an IMAP account now touches only that
  message.** Before, DAWN finished the move with a plain `EXPUNGE`, which
  permanently erased *every* message marked deleted in that folder, including
  ones another mail app had only marked. DAWN now uses `MOVE`, or removes just
  the one message. On a server that supports neither, the message is copied and
  marked deleted, and DAWN says it is still in the folder for your mail app to
  remove.
- **DAWN finds Trash and Archive by asking the server**, not by guessing names
  like "Trash" or "[Gmail]/Trash". It uses the folder your server marks for
  that purpose, or a folder named Trash, Deleted Items, Archive (and similar)
  at the top of your folders. It never uses a folder shared by other people.
- **If an account has no Trash folder, trash is refused** and the message stays
  where it is. Archive is refused the same way when there is no archive folder.
  Create the folder in your mail app if you want DAWN to use it.

**What you need to do.** Nothing. Gmail API accounts are unaffected.

---

## 2026-10-01 — Retrieved memory is sent once per conversation; two focus settings retired

**What changed.**
- **Each turn now sends only the retrieved items (memories, document passages,
  calendar events) the conversation hasn't already shown the model.** An item
  that is still relevant on a later turn is named on a short `[still relevant:
  M3, M7]` line instead of being sent again; an item whose text changed is sent
  again under the same number. DAWN reads this from the conversation itself, so
  it holds across a restart, a reload, a compaction and a forget (a forgotten or
  summarized-away item counts as not shown). Long conversations on one topic
  send far less, and the model's prompt cache holds more of each request.
- **`recent_window_turns` and `score_uplift_factor` under
  `[memory.focus_injection.dedup]` are retired.** They tuned the old "don't
  re-send for N turns" rule, which this replaces. If your `dawn.toml` sets them,
  DAWN logs once that they are ignored, and the next save from the WebUI
  settings panel drops the section. The two controls are gone from the panel.
- **The rules for retrieved items moved into the system prompt.** Each
  existing conversation gets one "updated instructions" message on its next
  turn, once.
- **The database moves to schema v100**, automatically on start. The memory
  citation audit records the items a turn named again apart from the ones it
  sent, and each conversation's compaction summary is now stored exactly as it
  is sent (cleaned once, when it is made, instead of at every reload). A
  summary that DAWN's stricter cleaning changes is updated once; that
  conversation's earlier turns then replay without the model's earlier
  reasoning (its text and tool calls stay), the same as after any other
  change that rewrites what a conversation was sent. The log line
  `v100 stored N compaction summaries as sent` says how many.
- **Text DAWN didn't write is cleaned more thoroughly.** Imitations of the new
  item lines are defused in tool results, retrieved items, attached documents
  and MCP tool descriptions. An MCP tool's description is now one line; a
  conversation using that tool gets the new description as a tool change,
  once, at its next turn (all of one server's tools together; if that server's
  tools already changed four times in the past hour, within the hour). A
  conversation whose tools have already changed 32 times keeps the
  descriptions it has, as it does for any later tool change.

**What you need to do.** Nothing. Optionally remove the
`[memory.focus_injection.dedup]` section from `dawn.toml` to silence the notice.
The WebUI Context panel now says, for each item, whether it was sent this turn
or was already in the conversation (reload the page once to pick up the new
script).

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

## 2026-10-01 — Camera captures stay in the conversation; `capture_history_count` retired

**What changed.**
- **An image a tool returns (a camera capture from `viewing`, an MCP image) is now
  kept with its conversation** in DAWN's image store, inside the tool's result, and
  reloads with the conversation. Before, a capture lived only in memory and older
  ones were blanked out as new ones came in. Captures are private to their owner and
  are deleted with their conversation. Guests' captures stay in memory only.
- **`[vision] capture_history_count` is retired and ignored.** If your `dawn.toml`
  sets it, DAWN logs one warning at startup and carries on; the next settings save
  removes it. Images are now bounded by the model instead: a request carries at most
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

**What you need to do.** Nothing, as long as the disk has the space above. A
manual `VACUUM` (the admin compact command) briefly needs twice the database's
size on disk, as before.

**Note on deleted data.** Deleting a conversation removes it from the database
file promptly (its space is returned to the disk). Like any deleted file, its old
contents can remain in freed disk blocks, in the database's log until it is
overwritten, and in any backup copies (DAWN's pre-upgrade backups in
`backups/`, and any copies you made yourself). Full-disk encryption is the way
to protect data at rest on the device.

---

## 2026-09-29 — Long conversations are compacted between turns

**What changed.**
- **Compaction happens between turns only.** When a conversation grows long,
  DAWN summarizes its oldest part in the background and puts the summary in
  place when your next message starts, never in the middle of a reply that is
  using tools. The summary now appears as a "CONVERSATION SUMMARY" block in
  front of the first kept message, the same whether the conversation is live
  or reopened.
- **Instructions survive a compaction.** A persona or setting change made in
  the part that was summarized is sent again, so the model keeps following it.
- **A reply that fills the conversation mid-task** now answers with what it
  has (or says it ran out of room) instead of compacting mid-reply; send another
  message to carry on. A background job continues on its own.
- **Switching models** fits the conversation to the new model with your next
  message.
- `context_expand` with no arguments shows the messages the latest summary
  replaced.

**Upgrading.** Nothing to do: the database migrates itself (schema v96).
Conversations that were already compacted lose the model's earlier reasoning
once (their text and tool calls stay).

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
