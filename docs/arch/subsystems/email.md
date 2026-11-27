# Email Subsystem

Source: `src/tools/email_*.c`, `src/tools/gmail_*.c`; the WebUI: `src/webui/webui_email*.c`, `src/webui/email_cursor.c`, `src/webui/email_wire.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Multi-account email via IMAP/SMTP and Gmail REST API, with voice-controlled send, read, search, trash, and archive.

## Architecture: Dual Backend + Service Router

```
┌───────────────────────────────────────────────────────────────────────┐
│   LLM TOOL INTERFACE              │  WEBUI MAIL PANEL (Aurora)        │
│  email_tool.c                     │  webui_email_panel.c: email_list, │
│  recent | read | search | folders │   _search, _read, _set_flags,     │
│  | digest | send | confirm_send   │   _unread_counts (email_cursor.c, │
│  | accounts | trash | archive     │   email_wire.c: paging, frames)   │
│  | confirm_trash                  │  webui_email_panel_move.c:        │
│                                   │   _archive, _trash, _undo         │
│  → TOOL_CAP_DANGEROUS gates       │  webui_email_exec.c: 4 workers,   │
│                                   │   one task per account            │
├───────────────────────────────────────────────────────────────────────┤
│                     SERVICE LAYER                                     │
│  email_service.c, email_service_read.c, email_service_move.c        │
│  → Trash/archive batches + undo (email_undo.c tokens, 60 s)          │
│  → Multi-account routing (dispatches to correct backend per account) │
│  → Two-step confirmation for send and trash (draft → confirm)        │
│  → Per-account read-only flag                                        │
│  → Pagination for large result sets                                  │
├───────────────────────────────────────────────────────────────────────┤
│              BACKEND A                     BACKEND B                  │
│  email_client.c (IMAP/SMTP)    gmail_client.c (Gmail REST API)      │
│  email_imap_read.c (read)      gmail_read.c, gmail_parts.c          │
│  email_imap_batch.c (trash,    gmail_flags.c (read/unread, counts)  │
│   archive, undo; COPYUID)      gmail_move.c (trash, archive, undo)  │
│  email_imap_move.c (folder roles)                                    │
│  email_imap_flags.c (\Seen, STATUS); one IMAP login per account at a │
│   time: email_account_lease.c                                        │
├───────────────────────────────────────────────────────────────────────┤
│                     READING A MESSAGE                                 │
│  email_mime.c (GMime) + email_display.c: one policy, both backends   │
│  email_transfer.c: failure codes, cancelling a transfer              │
│  → libcurl for IMAP/SMTP       → OAuth Bearer + XOAUTH2             │
│  → App password or XOAUTH2     → REST endpoints for all operations   │
│  → Any IMAP provider           → Google-specific (thread model)      │
├───────────────────────────────────────────────────────────────────────┤
│                     SQLITE STORAGE                                    │
│  email_db.c                                                          │
│  Tables: email_accounts (encrypted passwords via crypto_store)       │
│  → Shares auth_db SQLite handle                                      │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Design Points

- **Dual backend**: IMAP/SMTP for any provider, Gmail REST API for OAuth accounts (auto-selected per account).
- **Two-step confirmation**: send and trash require a confirm step — the LLM drafts, then the user confirms.
- **Contacts integration**: recipient resolution via `contacts_find()` — "email Bob" resolves to Bob's stored email.
- **Compile-time gate**: `DAWN_ENABLE_EMAIL_TOOL=ON` in CMake; the model gets the tool where `[llm.tools]` `local_enabled` / `remote_enabled` lists `email` (as for every dangerous tool).
- **WebUI management**: account CRUD via `webui_email.c`, Google OAuth connect flow.

## Recipients

A `to` that names a person is resolved by the same rules as a phone call or text
(`contact_resolve.c`; see [PHONE_SMS_DESIGN.md](../../PHONE_SMS_DESIGN.md#who-a-call-or-text-goes-to-contact-resolution)):
a partial name, a near-miss or a name the user never said still makes a draft, but the draft
says to check the recipient first. A literal `to` is exactly one address (a list is refused).
Display names are quoted in the To and From headers (`email_format_mailbox`).

## Reading a message

Both backends end in one reader, `email_mime.c`, the only file that uses GMime.
The backends hand over the message in different forms:

- **IMAP** fetches the raw RFC 822 bytes, bounded: 512 KB for the LLM tool, 2 MB
  for the mail panel's read (`EMAIL_READ_FETCH_PANEL`). A larger message comes
  back cut and reads as truncated; it is never refused. A read-specific curl sink stops the transfer at
  the cap.
  - **Pre-scan.** Before GMime sees the bytes, `email_mime_prescan` cuts the
    message at the first limit it passes. It counts boundary lines, and every line
    of every header block (the message's, each part's, and a forwarded message's,
    whatever the line looks like). It also caps one header's folded length and a
    line's length. GMime builds a message's whole tree before anything can stop it,
    and a few MB of tiny parts or headers can cost hundreds of MB to parse.
    Measured on a Jetson AGX Orin with GMime 3.2.7, without the pre-scan: 200,000
    tiny parts in 1.95 MB took 262 MB of RSS, a million header lines in 5.9 MB took
    492 MB, and a deep nest about 2 s of CPU. GMime's own nesting limit is 1,024
    levels; the walk over the parsed tree stops at 16 (`EMAIL_MIME_MAX_DEPTH`), but
    by then the tree is built, so only the pre-scan bounds what a parse costs. Its
    limits are in `email_mime.h` (1,024 boundary lines, 5,000 header lines, 64 KB
    for one header, 1 MB for one line).
  - **Address headers.** GMime's address parsing costs time quadratic in the
    entries without an '@' (and group colons), and it re-parses every header of an
    address type when another arrives. The pre-scan counts such entries across all
    address headers, skipping quotes and comments as GMime does, and cuts past
    1,000; a header block may hold at most 4 headers of each type GMime parses.
    Gmail's header values, which skip the pre-scan, are each cut at 1,000 such
    entries (`email_mime_addr_list`).
  - **No-copy parse.** GMime reads over the fetched buffer with `persist_stream` on,
    so a part's content is a window on that buffer, not a copy. The buffer outlives
    the parse because the parse is synchronous. Measured on a 25 MB message: 29 MB
    of RSS and 24 ms, against 79 MB and 65 ms with persist off over a copying
    stream.
- **Gmail** reads `format=full`. That gives the MIME tree with each text part's
  bytes inline and every attachment behind an `attachmentId`, so a read never
  downloads attachment bytes. `gmail_parts.c` (pure, no network) turns the tree
  into the same part list. `gmail_read.c` fetches, on its own, a body part that
  Gmail moved behind an `attachmentId`, and only one the policy will read.
  Nothing is cached between reads. (Attachment download, which would map a part
  id back to its `attachmentId` the same way, is planned for the email panel and
  not built.)
  - **Why not `format=raw`.** Raw downloads every attachment on every read: the
    LLM tool reading a thread with a 20 MB invoice would pull ~27 MB to say two
    lines. It needs a response buffer of ~40 MB; a raw read as first written cost
    100-150 MB of memory, and a hostile multipart could push one past 3 GB. And it
    loses the per-part `attachmentId` that fetching one part later needs. The
    structured fetch keeps reads small, which matters most on voice.

The part list includes the multiparts as nodes, so the policy (`email_mime_apply`)
sees the message's structure. It does the rest:

- **Picks the body from the structure.**
  - Of a multipart/alternative it reads one branch: the one with text/plain, else
    the HTML one turned into text. The other branches are the same body in
    another form, not attachments.
  - Of a mixed or related multipart it reads every inline text part in order.
    So a mailing list's HTML post comes before its plain footer, and a
    forward's cover note comes before the forwarded text.
  - A part with a filename is a file, even when it's marked inline.
  - HTML inside a text/plain part becomes text.
- **Decodes it to UTF-8** through GMime stream filters, bounded: a large part is
  never decoded whole, and a cut never splits a character.
  - Charsets on an allowlist convert as declared (compared in GMime's canonical
    spelling). Any other declared charset is read as windows-1252.
  - Undeclared 8-bit text is kept as UTF-8 when it is valid, else read as
    windows-1252.
  - Declared UTF-8 keeps its valid text; a bad byte becomes `?`.
  - Tag characters (U+E0000-E007F: invisible, but a model reads them) are
    removed. Line and paragraph separators become line breaks.
- **Produces HTML only on request.** `body_html` (raw; whoever shows it must
  sanitize it) exists only when `want_html` is set, capped by `max_html_bytes`.
  The LLM tool never asks for it; the mail panel's read does
  (`EMAIL_READ_HTML_PANEL`).
- **Reads only the text a person would see.** HTML becomes text through
  `html_extract_text_plain_ex`, which drops what a mail client hides: `hidden`,
  `display:none`, `visibility:hidden`, zero size, clipped or transparent text,
  `<head>` and the like, from inline styles and from the message's own `<style>`
  rules (a bounded cascade in `html_hidden.c` / `html_hidden_css.c`, with a work
  budget past which the rest reads as hidden). So an instruction styled out of
  sight doesn't reach the model. The message records that text was dropped
  (`hidden_text`). When the HTML shows no text (only images, or it couldn't be
  read), the plain-text version is used, with a note that the reader may not have
  seen it.
- **Lists every other leaf as an attachment**: part id, filename, type,
  Content-ID, size, and whether it's inline.
- **Sets truncation flags**: `text_truncated`, `html_truncated` and
  `attachments_truncated`.

Part ids are IMAP section numbers ("2", "2.1") on both backends, so a later
per-part fetch can use them directly.

**Read state.** The backends mark a read differently, and the read option `mark`
(`email_types.h`) evens them out. IMAP's body fetch goes through libcurl's URL form,
a plain `BODY[]` (`BODY.PEEK` can't be reached through it), so the server sets
`\Seen`; a Gmail read marks nothing. The tool's read leaves each as its backend does.
The mail panel's `email_read` (`mark_read`, default true) marks a Gmail message read
with a label change. With `mark_read: false` on IMAP it reads the flags first, on the
same login, and when the message was unread clears `\Seen` again right after the fetch
(`email_imap_restore_unseen`). The restore runs whatever became of the fetch (failed,
cut or stopped: it may have set the flag before it ended), with the cancel dropped and
a 10 s timeout of its own. Known side effects: it also clears a `\Seen` another client
set in that moment; after an aborted transfer libcurl may have dropped the connection,
so the restore can cost a second login; and if it fails the message stays read. The
response's `unread` says what actually happened, not what was asked.

The email tool's read, recent, search and digest results reach the model framed as `EMAIL CONTENT`
(third-party text, after the neutralizer; see [llm.md](llm.md), Tool results), from the shared
formatter `email_render.c` for a read, and memory extraction never learns from them.

Everything taken from a message is the sender's text. Subjects, names, addresses
and filenames go through `email_display_sanitize`: well-formed UTF-8 only, with
controls, bidi overrides and isolates, and zero-width characters removed.
Content-IDs are limited to RFC 5322 atext.

A trash confirmation reads headers only. On IMAP that is `FETCH (ENVELOPE)`,
which doesn't mark the message read; on Gmail it is `format=metadata`.

## Searching every account

A tool `search` with no account searches every enabled account at once, one thread
each (`email_fanout.c`, joined before the call returns; Stop ends them all, since
each runs under the caller's cancel flag). An IMAP account busy elsewhere for 10 s
is reported, not waited out. The rows are merged newest first by date across the
accounts, and the limit keeps the newest; each failed account is reported. One
log line per account gives its time, which names a slow server.

The mail panel pages differently: its tasks run on the email executor, and its
merge (`email_merge_page`) keeps each account's own order, comparing only each
account's next row, because its cursor must record an exact position per account.
The tool has no cursor across accounts, so it can sort freely.

**Gmail lists in two steps**: the ids (`messages.list`), then each message's
headers in batch requests of 20 (`gmail_batch.c`). Gmail refuses some parts of a
batch when a user's requests come fast (429, or a 403 naming a rate limit); those,
and server errors, are asked for again after 1 s and then 2 s. A 401 or another
403 isn't retried. Rows keep the listing's order. Any still refused are counted, not
dropped: the tool says the list is incomplete, the panel marks the account partial
(RATE_LIMITED), the digest notes it, and reply states stay unknown. Search terms
lose quotes and currency signs, with which Gmail matches nothing.

## The mail panel: lease, executor and cursor

- **The lease is IMAP only.** IMAP servers cap connections per account, so every
  caller (the tool, the digest, the panel) takes the account's lease around a whole
  operation (`email_account_lease.c`). Gmail is HTTP with per-user quotas and no
  connection cap, so its tasks run in parallel without one (moves are paced, below).
- **Why tickets and FIFO handover.** A design in which a multi-account request waited
  for its per-account tasks, and workers blocked waiting for a lease, could tie up all
  four executor workers waiting on work only a free worker could run. The fix has
  three properties:
  - A request across N accounts is N tasks sharing a join; the task that brings the
    join's count to zero merges and delivers. No task ever waits on another.
  - A release hands the lease straight to the head waiter, so no waiter misses a
    release and a blocking thread can't always beat a queued task.
  - An executor task that can't have the lease parks as a ticket in the lease's FIFO
    and the worker moves on. The release hook (called with no lease lock held) queues
    it again, already holding the lease. Only callers outside the executor (the tool,
    the digest) block, in 250 ms slices that check their cancel and deadline.
- **The first inbox page carries the unread counts**, so opening the panel costs one
  login per account, not two (the target: one login per account to open the panel,
  one per read). IMAP sends `STATUS INBOX (UNSEEN)` on the list's own login before any
  mailbox is selected: RFC 3501 advises against STATUS on the selected mailbox. Gmail
  reads `labels/INBOX`. Searches don't carry counts.
- **IMAP rows have no preview.** libcurl's custom-request FETCH passes response lines
  through but discards literals, and message text comes back as a literal, so the
  list can't read it on the same command.
- **The cursor is the client's and untrusted.** Its filter hash is deliberately
  unkeyed: it only says which request the cursor belongs to. Trust comes from
  rechecking at run time: every account id against the user's enabled accounts (again
  inside the task, which resolves the account afresh), every position through its
  parser. The hash binds the account set, so an account the request covers but the
  cursor leaves out is done, never "start over".

## Moving messages (trash, archive) and undo

`email_service_move.c` moves up to 50 messages of one account per call, to the
folder the server marks for the role (`\Trash`, `\Archive`; `\All` on Gmail), or
else a known name at the top of the user's folders (`email_imap_move.c` finds and
caches the roles). It never uses another user's or a shared namespace.

- **IMAP ids are pinned.** An id is `folder:uid.v`, `v` the mailbox's UIDVALIDITY,
  and every SELECT for it carries `;UIDVALIDITY=v`: a mailbox rebuilt since the id
  was issued answers NOT_FOUND rather than the wrong message. Old ids without
  `.v` still work, unpinned.
- **The IMAP move** (`email_imap_batch.c`): one login per call, ids grouped by
  (folder, epoch). Per group a read-only `UID FETCH` confirms which exist, then
  `UID MOVE` of those, else `UID COPY` + `STORE \Deleted` + `UID EXPUNGE` of those
  UIDs (no UIDPLUS: left marked deleted). A bare `EXPUNGE` is never sent.
  Stopping is honoured between groups only.
- **Where each message landed** comes from COPYUID (RFC 4315), read only while
  the MOVE or COPY is in flight and only from its own tagged reply or untagged
  lines, mapped only onto UIDs DAWN sent. The window matters because libcurl hands
  the debug callback the lines inside IMAP literals too: outside it, a subject
  carrying CRLF and `* OK [COPYUID ...]` in a FETCH could say where messages went.
  UIDVALIDITY is read by the same rule, only while a SELECT's answer comes in.
- **A refused SELECT looks like a refused login.** libcurl reports a SELECT the
  server refuses (a folder that's gone) as `CURLE_LOGIN_DENIED`. On a connection
  already logged in no LOGIN goes out, so an unchanged on-wire login count across
  the command means it was the folder (`email_imap_select_failed`): that group's ids
  are NOT_FOUND, not an auth failure.
- **Gmail** (`gmail_move.c`): per message, labels read first (already there, or
  an archive from Trash/Spam, is answered without moving), paced to 40 calls a
  second per account from one bucket the tool and the panel share (Gmail takes no
  lease), so a 50-message move takes about 2.5 s instead of bursting past Gmail's
  per-user rate.
- **No folder for the role.** The move is refused, never guessed; a role folder
  removed since (TRYCREATE) is forgotten and refused the same way. Only those
  forget the cached roles: a transient MOVE or COPY failure leaves them, since
  wiping them made the account's trash and archive buttons flap.
- **Undo** (`email_undo.c`): a move from the panel gets a token per message,
  random, single use, bound to the user, the account and its server and login,
  kept 60 s in memory. An undo checks the message is still where it went, moves it
  back, and reads its new row. Every move and undo is told to the user's panels
  once (`email_changed_notify`).
  - An unknown, expired, foreign or in-flight token all answer `UNDO_EXPIRED`,
    alike on purpose, so a token can't be probed for whose it is. The store is
    memory only: after a restart every token answers the same. At most 128 per
    user and 512 in all; a full store gives the move no undo rather than evict
    another user's.
  - A Gmail trash's record keeps the labels an undo can add back: INBOX, UNREAD,
    STARRED, IMPORTANT, `CATEGORY_*` and user labels, never TRASH, SPAM, SENT,
    DRAFT or CHAT (they would put the message where it wasn't moved from, or the
    API can't add them), packed into at most 384 bytes (labels past it are dropped
    and logged). The undo untrashes, then adds them back; on a 400 (a user label
    deleted since) it retries with the system labels only. An archive's undo adds
    INBOX.
  - Decisions: a 60 s window. No undo on an IMAP server without UIDPLUS (no
    COPYUID says where the message went), and no Message-ID search to find it
    instead. Rapid clicks are batched by the client into one call; the server never
    merges requests.
- **What an account can do.** The panel's account list says whether each account
  can trash and archive (`email_service_account_caps`). Gmail always can; for
  IMAP it's whether the server has a folder for the role, learned from a move or,
  at most once an hour per account, from the account's first inbox page. A probe
  that fails is tried again on the next page. Each cached entry has a generation
  number: removing or changing the account forgets it, and a probe that started
  before that can't write its answer back.
- **The panel's moves run in order.** Archive, trash and undo go to the
  executor's MOVE slot: one running and three queued per session, first in
  first out, and a newer request never replaces a queued one. A fifth is BUSY.
  A move that started always runs its login and its first folder (IMAP) or
  message (Gmail) with no stop armed, so it always acts and always sends
  `email_changed`; a stop waits at most for that, and a stop between folders
  leaves the rest where they were.

### Not built yet: live changes

The panel refreshes by hand; pushing changes made elsewhere (another mail client,
new mail) is future work, and three things wait for it. IMAP list rows get a preview
then, from the snapshot that work keeps, since the list itself can't read one. The
error `CANNOT_CALCULATE` (the changes since a given state can't be worked out) is
reserved for it and sent by nothing today. And `email_changed` goes to every browser
tab of the user, the one that asked included, until tabs can subscribe to the
accounts and folders they show.

## Email in chat

A WebUI `text` turn may name one email the user attached (`email_refs`, flag `email_refs`;
`docs/WEBSOCKET_PROTOCOL.md`, Email in chat). `src/webui/webui_email_ref.c` checks it at
receipt in the database only: the account is the user's and enabled, the message id has a
tool id's shape, and the name the model will use for the account resolves back to that same
account (`email_service_find_account_by_name`). Nothing is fetched then. The turn's context
names the email (`session_turn_attach_email`), and the model reads it with the email tool's
`read`, so its text arrives as a framed tool result (`EMAIL CONTENT`), never as the user's
words (`docs/PROMPT_INJECTION.md`). In that turn no confirm counts and act/device calls are
refused (`TURN_ORIGIN_THIRD_PARTY`, `TOOL_CALLER_THIRD_PARTY`). The question row keeps
`messages.email_ref` (account id, message id, and the client's from/subject for the chip,
which never reach the model).

## Confirming a send or a trash

Sending and trashing are two steps: the tool prepares a draft or a pending
trash, and a second call confirms it. The approval is conversational by
design: on voice, the model reads the action back and the user says yes. So
the confirm comes from the model, and the risk to design against is the model
confirming without a person's yes. That can happen when text it read (an email,
a web page, a tool result) tells it to.

**What makes a confirm count:**
- **The same session.** A draft records the session and the turn that prepared
  it (`turn_origin_t` in `include/core/turn_origin.h`, the rule every confirm
  shares). A confirm from another session (another browser tab, device or
  channel) is refused, even by the same user. The binding is the session, not
  the conversation.
- **The next turn.** The confirm must run in the session's very next turn
  after the draft, and in a turn the user started. The model can't prepare
  and confirm in one turn, and can't confirm later once the user has moved on.
  Only the user's reply to the read-back counts. If a background turn runs in
  between, the confirm is refused (the safe direction), and the model prepares
  the action again.
- **The user's own turn.** The tool's actions that send, delete or move mail
  (`send`, `confirm_send`, `trash`, `confirm_trash`, `archive`) need a running
  turn the user started, and are refused anywhere else: in a background job, in
  a background (re-engaged) turn, in a scheduled run (the scheduler refuses
  them too), and from an MQTT message, with or without a session named.

A refused confirm leaves the draft in place, and it doesn't count toward the
wrong-id throttle.

**The read-back.** A draft's result gives the model one fixed line to say as
written, "Sending to <address>, from <account>, subject: <subject>", so the
user hears where the mail really goes.

**What remains:**
- Any turn begun in the session between the read-back and the yes (a finished
  background job's reply in that tab, a voice clip that heard nothing) makes the
  yes too late. The model is told to prepare the action again. This fails safe.
- The user's next message satisfies the rule whatever it says, not just "yes".
  Text the model read could still lead it to confirm after an unrelated reply,
  or after a message the user typed before the read-back arrived.
- The model relays the read-back, so it could misstate it.
- Drafts expire after five minutes.
- Accounts can be read-only in DAWN, and the tool can't send from them.
- Trash is recoverable (it moves; it never expunges).
- Send is not recoverable. It is the case these rules are for.
- On messaging channels, "the user" is the person who linked the channel
  (a group chat answers only them, though everyone in it reads the replies).
  An SMS sender's number can be forged, though, so over SMS "the user" is
  whoever can put that number on a text.
