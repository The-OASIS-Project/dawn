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
│  | send | confirm_send | accounts │   _unread_counts (email_cursor.c, │
│  | trash | confirm_trash | archive│   email_wire.c: paging, frames)   │
│                                   │  webui_email_panel_move.c:        │
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
- **Compile-time gate**: `DAWN_ENABLE_EMAIL_TOOL=ON` in CMake; runtime gate in `[email] enabled`.
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
  - **Address headers.** GMime's address parsing costs time quadratic in the
    entries without an '@' (and group colons), and it re-parses every header of an
    address type when another arrives. The pre-scan counts such entries across all
    address headers, skipping quotes and comments as GMime does, and cuts past
    1,000; a header block may hold at most 4 headers of each type GMime parses.
    Gmail's header values, which skip the pre-scan, are each cut at 1,000 such
    entries (`email_mime_addr_list`).
  - **No-copy parse.** GMime reads over the fetched buffer with `persist_stream` on,
    so a part's content is a window on that buffer, not a copy.
- **Gmail** reads `format=full`. That gives the MIME tree with each text part's
  bytes inline and every attachment behind an `attachmentId`, so a read never
  downloads attachment bytes. `gmail_parts.c` (pure, no network) turns the tree
  into the same part list. `gmail_read.c` fetches, on its own, a body part that
  Gmail moved behind an `attachmentId`, and only one the policy will read.
  Nothing is cached between reads. (Attachment download, which would map a part
  id back to its `attachmentId` the same way, is planned for the email panel and
  not built.)

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
  lines, mapped only onto UIDs DAWN sent.
- **Gmail** (`gmail_move.c`): per message, labels read first (already there, or
  an archive from Trash/Spam, is answered without moving), paced to 40 calls a
  second per account.
- **No folder for the role.** The move is refused, never guessed; a role folder
  removed since (TRYCREATE) is forgotten and refused the same way.
- **Undo** (`email_undo.c`): a move from the panel gets a token per message,
  random, single use, bound to the user, the account and its server and login,
  kept 60 s in memory. An undo checks the message is still where it went, moves it
  back, and reads its new row. Every move and undo is told to the user's panels
  once (`email_changed_notify`).
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
