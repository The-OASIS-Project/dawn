# Email Subsystem

Source: `src/tools/email_*.c`, `src/webui/webui_email.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Multi-account email via IMAP/SMTP and Gmail REST API, with voice-controlled send, read, search, trash, and archive.

## Architecture: Dual Backend + Service Router

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  email_tool.c                                                        │
│  Actions: recent | read | search | folders | send | confirm_send     │
│           | accounts | trash | confirm_trash | archive               │
│  → TOOL_CAP_DANGEROUS: compile-time + runtime gates                  │
├───────────────────────────────────────────────────────────────────────┤
│                     SERVICE LAYER                                     │
│  email_service.c, email_service_read.c                               │
│  → Multi-account routing (dispatches to correct backend per account) │
│  → Two-step confirmation for send and trash (draft → confirm)        │
│  → Per-account read-only flag                                        │
│  → Pagination for large result sets                                  │
├───────────────────────────────────────────────────────────────────────┤
│              BACKEND A                     BACKEND B                  │
│  email_client.c (IMAP/SMTP)    gmail_client.c (Gmail REST API)      │
│  email_imap_move.c (trash,     gmail_read.c, gmail_parts.c          │
│   archive)                                                           │
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

## Reading a message

Both backends end in one reader, `email_mime.c`, the only file that uses GMime.
The backends hand over the message in different forms:

- **IMAP** fetches the raw RFC 822 bytes, bounded: 512 KB for the LLM tool, 2 MB
  for the WebUI mail panel. A larger message comes back cut and reads as
  truncated; it is never refused. A read-specific curl sink stops the transfer at
  the cap.
  - **Pre-scan.** Before GMime sees the bytes, `email_mime_prescan` cuts the
    message at the first limit it passes. It counts boundary lines, and every line
    of every header block (the message's, each part's, and a forwarded message's,
    whatever the line looks like). It also caps one header's folded length and a
    line's length. GMime builds a message's whole tree before anything can stop it,
    and a few MB of tiny parts or headers can cost hundreds of MB to parse.
  - **No-copy parse.** GMime reads over the fetched buffer with `persist_stream` on,
    so a part's content is a window on that buffer, not a copy.
- **Gmail** reads `format=full`. That gives the MIME tree with each text part's
  bytes inline and every attachment behind an `attachmentId`, so a read never
  downloads attachment bytes. `gmail_parts.c` (pure, no network) turns the tree
  into the same part list. `gmail_read.c` fetches, on its own, a body part that
  Gmail moved behind an `attachmentId`, and only one the policy will read. A
  later attachment download re-reads the tree and maps a part id back to its
  `attachmentId` the same way; nothing is cached between reads.

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
- **Produces HTML only on request.** `body_html` (raw, for the panel, which
  sanitizes it) exists only when `want_html` is set. The LLM tool never asks for it.
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

## Moving a message (trash, archive)

`email_imap_move.c` moves exactly one message, to the folder the server marks
for the role (`\Trash`, `\Archive`; `\All` on Gmail), or else a known name at
the top of the user's folders. It never uses another user's or a shared
namespace.

- **The move.** `UID MOVE`, else `UID COPY` + `STORE \Deleted` + `UID EXPUNGE` of
  that one UID. A bare `EXPUNGE` is never sent.
- **Before it.** A read-only `UID FETCH` first confirms the message exists.
- **No folder for the role.** The move is refused, never guessed.

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
  them too), and from an MQTT message, with or without a session named. A
  build without multi-client support has no turns to check, and it can't tell
  the local mic from MQTT, so it refuses these actions altogether.

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
