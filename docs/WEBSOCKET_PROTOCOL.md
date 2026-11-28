# DAWN WebSocket Protocol Reference

This document describes all WebSocket message types used between DAWN daemon,
WebUI browser clients, and DAP2 satellite devices. All connections use the
`dawn-1.0` WebSocket subprotocol on the same port (default 3000).

## Transport

- **Text messages**: JSON with `{"type": "...", "payload": {...}}`. A member whose value
  is `null`, at any depth, is treated as absent (`null` elements of an array stay).
- **Binary messages**: Single type byte prefix followed by raw data
- **Subprotocol**: `dawn-1.0` — **mandatory, and it fails silently.** libwebsockets
  selects its protocol handler by subprotocol name, so a client that negotiates none
  is routed to the **HTTP** handler instead: the socket opens cleanly, `send()`
  succeeds, and every frame is discarded with **no error and no server log line**.
  The symptom is a connection that looks healthy and never answers. Non-browser
  clients must pass it explicitly (Python `websocket-client`:
  `create_connection(url, subprotocols=["dawn-1.0"])`). Reference client:
  `tests/tools/tail_conversation.py`.
- **Authentication**: HTTP cookie set during login (see `webui_http.c`). Obtain it
  with `GET /api/auth/csrf` then `POST /api/auth/login`; the WebUI is TLS-only when
  `[webui] ssl_cert_path` is set, so use `wss://`. A session belongs to the login
  that created it: a `reconnect` token only reattaches from that same login.
- **One login per app** (feature `app_logins`): a front-end other than the WebUI names
  itself on login (`"app": "aurora"` in the JSON body; `[a-z0-9_]`, 1-16 characters)
  and gets its own cookie, `__Host-dawn_session_<app>` (the WebUI's is
  `__Host-dawn_session`; the `__Host-` prefix means the browser only accepts it from
  this host, over HTTPS, with `Path=/` and no `Domain`). Every
  other request says which app it is from with **`?app=<app>` on its URL**: the
  WebSocket and music-socket upgrade URLs, fetches, image and document links. No `app`
  means the WebUI; an invalid or unreadable one means no login, never the WebUI's.
  URLs DAWN sends (image and document links in frames) carry no `app`: a front-end
  adds its own `app=` to each one (merged with `&` when there is a query already).
  So the WebUI and another front-end
  open in the same browser keep separate logins: logging out of or into one never
  touches the other.
- **How long a login lasts** (feature `login_expiry`): it ends about **a week after its
  last use** (6 to 7 days: use moves the expiry at most once a day), and never more than
  **30 days** after sign-in. Use means a message the user's
  own action sent (a turn, push-to-talk or an always-on voice command, a music control, a
  settings save, and the like); the
  client's `ping`, `init`/`reconnect` and the requests it makes by itself on connect
  don't count, so an unattended tab still ends. An "always on" login
  (`session_keepalive_enable`) is moved on by its `ping` instead. "Remember me" only
  keeps the cookie past the browser closing. The expiry, `expires_at` (epoch seconds),
  is on the `session` frame, every `pong`, `GET /api/auth/status` when logged in, and a
  `login_expiry` frame each time use moves it;
  use moves it at most once a day, so a client warning shortly before it ends sees the
  real time.
- **Logging out**: `POST /api/auth/logout` (with `?app=<app>` for another front-end;
  same-origin: `Origin`, or a `Referer` whose `scheme://host[:port]` is exactly this
  server's or on `[webui] allowed_origins`) **with the socket open**. It ends that app's
  login only. The reply is immediate: `200 {"success":true}`, that app's cookie cleared
  (`Max-Age=0`), `Cache-Control: no-store`. Every connection on that
  login gets `force_logout` and is closed with **`4002`**, its music socket is closed
  by the server, and its sessions are destroyed, connected or not. A login over an
  existing one of the same app in the same browser ends the old one the same way.
- **When the login can't be checked** (the daemon's database is down or busy, as
  in a restart): it isn't reported as ended. An authenticated request draws the
  error `SERVICE_UNAVAILABLE` ("Couldn't check the login; try again") and the
  connection stays logged in; `GET /api/auth/status` and the other cookie-checked
  HTTP routes answer **`503`** (a JSON `{"error":"unavailable"}` from the JSON
  endpoints, the server's own 503 page otherwise: key on the status, not the body);
  a socket that connects then is closed with **`1013`** (try again later). At the
  start of a shutdown every socket is closed with **`1012`** (service restart), new
  ones are refused, and every HTTP request, `/api/auth/status` included, gets the
  503 page.
  Reconnect on all of these; `UNAUTHORIZED`, `"authenticated":false` and `4002`
  mean the login really ended (with the `login_unavailable` flag).

## Binary Message Types

| Byte | Direction | Name | Description |
|------|-----------|------|-------------|
| `0x01` | Client → Server | `AUDIO_IN` | Opus-encoded audio chunk (voice input) |
| `0x02` | Client → Server | `AUDIO_IN_END` | End-of-utterance marker (empty payload) |
| `0x11` | Server → Client | `AUDIO_OUT` | Opus-encoded TTS audio chunk |
| `0x12` | Server → Client | `AUDIO_SEGMENT_END` | End of TTS sentence segment (play now) |
| `0x20` | Server → Client | `MUSIC_DATA` | Opus-encoded music audio chunk |
| `0x21` | Server → Client | `MUSIC_SEGMENT_END` | End of buffered music segment |

Audio format: 16-bit PCM at 16kHz mono (raw), Opus-encoded for WebSocket transport.
Music format: Opus-encoded at 48kHz stereo.

---

## Protocol version and feature flags

DAWN advertises what its protocol does, so a client can adapt to the server it meets
instead of guessing from the version string. Both carriers say the same thing: the
`config` frame (sent on every connect and reconnect) and `GET /api/auth/status`
(logged in or not, for decisions made before a socket opens):

- `protocol` (integer): the protocol version. Bumped **only** when something is removed
  or changes incompatibly, after a deprecation window. A client that doesn't speak the
  server's version says so ("this DAWN is newer/older than this client") rather than
  half-working.
- `features` (array of strings): behaviours a client must know about to use them.
  Names are stable snake_case; a flag isn't removed while the protocol version stands.
  **A missing `features` array means a DAWN from before flags existed:** assume only
  the legacy behaviour.

When to add what:

- A new optional field: nothing. Clients detect it by its presence.
- A new or changed behaviour, or a new request a client must opt into: a feature flag.
- A removal, or a field whose meaning changed incompatibly: a protocol bump, after a
  deprecation window.

| Flag | Since | Meaning |
|------|-------|---------|
| `login_expiry` | 2026-11-25 | A login ends about a week (6 to 7 days) after its last use (a message the user's own action sent; heartbeats, reconnects and automatic requests don't count), never more than 30 days after sign-in; `expires_at` (epoch seconds) is on the `session` frame, every `pong`, `GET /api/auth/status` and a `login_expiry` frame when use moves it. See How long a login lasts. |
| `stopped_replies_saved` | 2026-11-18 | A reply the user stops (`cancel`, or `stop_other_reply` from another of their clients when the stopped turn is a WebUI client's) while it streams is saved as far as it had streamed: its `stream_end` has `reason` `"stopped"`, with `will_persist: true` when the row is saved (nothing is saved when nothing of the reply was left), and the row comes back with `stopped: true` on `message_appended`, `load_conversation_response` rows, `get_memory_fact_source_response` messages and exports. Show the saved text (it can differ from what was drawn: tags removed) and mark it stopped. Only the user's Stop does this; any other end of a turn is as before. Without it, a Stop mid-stream ends with `reason` `"error"` and saves nothing. |
| `memory_batches` | 2026-11-03 | The daemon records what memory learned per batch and answers `list_memory_batches`, `undo_memory_batch` and `keep_memory_op`, and pushes `memory_batch` (see Memory Management, Consolidation). Advertised while memory is enabled; a build without the WebUI never advertises it. Without it, offer no marker, Learned tab or undo. |
| `email_refs` | 2026-10-09 | A `text` frame may carry `email_refs`: one email the user attached, by account and message id. The model reads it with the email tool, and the question row comes back with `email_ref` on every frame that delivers it (see Email, Email in chat). Advertised with `email_client`. Without it, send no `email_refs` (an older daemon ignores them, and the model never sees the email). |
| `email_client` | 2026-10-07 | The mail panel's verbs answer: `email_list`, `email_search`, `email_read`, `email_set_flags`, `email_unread_counts`, `email_archive`, `email_trash`, `email_undo`, and the server pushes `email_changed` (see Email). Advertised only while the email service is up; a build without the WebUI or the email tool never advertises it. |
| `document_attachments` | 2026-10-02 | A `text` frame may carry its documents as `attachments` (`[{filename, size, content, blob_id?}]`): the daemon defuses each body and filename and builds the `[ATTACHED DOCUMENT: …]…[END DOCUMENT]` text itself, so a document can't end its own span. Without it, inline the documents into `text` as before. Every daemon with it also has `turn_refs`. |
| `turn_refs` | 2026-10-02 | A `text` frame may carry `client_ref`: the turn's own user `transcript` echo and every `error` raised for that turn (refused at receipt, refused or failed when it runs) carry it back unchanged, so a client knows which of its turns an error belongs to. Without it, refusals name no turn. |
| `visual_prompt_guard` | 2026-10-07 | A `text` frame may carry `from_visual: true` for a prompt a rendered visual sent; no confirm (email send or trash, call, text, document delete, door, research) counts in that turn. Send a visual's prompt only to a daemon with this flag, as its own turn; without it, drop the prompt and tell the user, never send it as an ordinary turn. |
| `image_only_turns` | 2026-10-01 | A `text` turn with no words (empty, absent or whitespace `text`) but at least one `image_ids` entry runs with just the images; one with neither is refused with `EMPTY_MESSAGE` instead of being dropped silently. Without it, require text with an image: older daemons drop a turn with no text without a reply. |
| `image_turns_by_id` | 2026-10-01 | A `text` turn takes images only through `image_ids`; `images[]` is ignored. A malformed, missing or foreign id, or too many, refuses the whole turn with `IMAGE_UNAVAILABLE` / `IMAGE_LIMIT` / `IMAGE_ERROR` and saves nothing (see `text`). Without it, send `images[]` with `image_ids` as before: older daemons send the model only `images[]`. |
| `app_logins` | 2026-09-30 | One login per app: `"app"` on login, `?app=` on every other request, a cookie per app (see Connection Lifecycle). Without it, all front-ends in a browser share one login. |
| `login_unavailable` | 2026-10-30 | A login the daemon couldn't check is never reported as ended: `SERVICE_UNAVAILABLE` on the socket (still logged in), `503` over HTTP, `1013` for a socket connecting then, and `1012` for every socket when a shutdown begins (see Transport, When the login can't be checked). So `UNAUTHORIZED`, `"authenticated":false` and `4002` mean the login ended. Without it, a daemon restart can look like a revoke: re-check `/api/auth/status` before signing out. |
| `request_refs` | 2026-11-16 | Any message may carry `req` in its payload (an opaque id: at most 64 bytes of valid UTF-8, no control characters; anything else is ignored). While the daemon handles the message, every `error` it sends to that connection carries the same `req` and `request_type` (the message's `type`, with or without a `req`), and every `*_response` it sends there carries `req`. So a refusal such as `UNAUTHORIZED`, `SERVICE_UNAVAILABLE` or `FORBIDDEN` names the request it refused, whatever order frames arrive in. Not covered: frames that aren't a `*_response` (`session` and the rest of the `init` / `reconnect` batch, `state`, `pong`, `music_state`), `restart_response`, a `text` turn's errors (queued, and matched by `client_ref`: flag `turn_refs`), and a reply sent later from another thread, which carries `req` only where its verb says so (the email verbs). Without the flag, match replies by type and order. |
| `turn_wait` | 2026-11-16 | One turn runs on a conversation at a time. A turn sent while another (another tab, another app, a background job, a messaging channel) runs on the same conversation waits for it: the sender gets `turn_waiting` (`waiting: true`) and then `turn_waiting` (`waiting: false`) when its turn starts, or `turn_unsent` if it was stopped first (nothing of it was saved). `stop_other_reply` stops the turn it waits for. Without the flag a client still gets a `state` `thinking` with a `detail` (`Finishing the reply already in progress…`) while it waits, then the normal stream; ignore the unknown frames. |
| `logout_closes_sockets` | 2026-09-30 | `POST /api/auth/logout` with the socket open is safe: the login's connections get `force_logout` and close with `4002`, the server closes the music socket, and the reply comes at once with the cookie cleared (see Connection Lifecycle, Logging out). Without it, close the socket before logging out. |

A client says who it is in its `init` / `reconnect` payload:
`"client": {"name": "aurora", "version": "1.4.0", "protocol": 1}`. DAWN logs it once per
connection (and warns on a protocol it doesn't speak); nothing is enforced on it.

## Connection Lifecycle

### WebUI Client Flow

1. HTTP login → cookie set → WebSocket connect
2. First message must be `reconnect` (with token) or any message (new session created).
   A connection with no login gets `UNAUTHORIZED` for it and is closed
3. Server responds with `session` + `config` + `state` messages
4. Client is ready for interaction

### Satellite (DAP2) Flow

1. WebSocket connect (no auth cookie)
2. First message must be `satellite_register`
3. Server responds with `satellite_register_ack`
4. Satellite is ready for `satellite_query` messages

---

## Client → Server Messages

### Core

#### `text`
Send a text message to the AI, with the images attached to it by id.
```json
{
   "type": "text",
   "payload": {
      "text": "What is in this picture?",
      "image_ids": ["img_a1b2c3d4e5f6"],
      "client_ref": "17",
      "attachments": [{"filename": "report.pdf", "size": 52113,
                       "content": "<extracted text>", "blob_id": "blb_a1b2c3d4e5f6"}],
      "email_refs": [{"account_id": 3, "message_id": "u4182.1700000000",
                      "from": "Bob <bob@example.com>", "subject": "Lunch Friday?"}]
   }
}
```
- `email_refs` — optional (flag `email_refs`); the email the user attached to this turn, at most
  one (see Email, Email in chat).
- `attachments` — optional (flag `document_attachments`); the documents the turn attaches,
  from the `POST /api/documents` upload: `filename` (1–255 bytes, no line break), `size` (the
  original file's bytes, a non-negative integer), `content` (the extracted text), and
  `blob_id` (the upload's `original_blob_id`; omit it when there is none, never `null`). At
  most `[documents] max_documents` (default 5) per turn, each `content` at most
  `[documents] max_extracted_size_kb` KB (default 1024). The daemon defuses DAWN's markers in
  each body and filename (they are someone else's text), quotes any document-marker line
  inside a body, and builds the turn's text as clients used to inline it: one
  `[ATTACHED DOCUMENT: <filename> (<size> bytes)[ blob:<blob_id>]]\n<content>\n[END DOCUMENT]`
  block per document, separated by a blank line, then a blank line and the words. The echo,
  the saved row and a reload carry that text. `text` holds only the words, and may be empty
  when the turn has attachments. A malformed entry, too many or too large, or a `blob_id` that
  is malformed or names no stored original of the user's refuses the turn with
  `ATTACHMENT_INVALID`. A client without the flag inlines the documents into `text` itself;
  the daemon still accepts that, defusing what it finds between the markers.
- `client_ref` — optional (flag `turn_refs`); an opaque tag of 1 to 64 printable ASCII
  characters (0x20–0x7e). The turn's own user `transcript` echo carries it as
  `payload.client_ref`, and so does every `error` raised for the turn, whatever its code: on
  receipt (e.g. `EMPTY_MESSAGE`, `IMAGE_*`, `TURN_QUEUE_FULL`, `UNAUTHORIZED`, `SESSION_LIMIT`)
  and while it runs (e.g. `IMAGE_*`, `LLM_ERROR`, `PERSIST_ERROR`). An error with the ref
  isn't always a failed turn: `PERSIST_ERROR` comes after the reply was shown (it wasn't
  saved), so read the code, and `severity`, not just the ref. Other frames of the turn (stream
  deltas, the reply, other viewers' copies) don't carry it. A `client_ref` that isn't a
  string of that shape (a number, empty, too long, or with other characters) refuses
  the frame with `INVALID_CLIENT_REF` (that error carries no ref).
- `from_visual` — optional, `true` only when the text is a rendered visual's prompt (a
  `render_visual` iframe's `sendPrompt` bridge), not the person typing. A visual is written by
  the model, and its script can run on load or after any click, so the daemon lets **no
  confirm** count in a turn it starts, whatever the words: an email send or trash, a call or
  text, a document delete, a door, a research run. The model is told to prepare the action
  again and wait for the person's own reply. A client should send bridge prompts this way,
  never by typing them into the user's draft, and only after a click (a first filter). Any
  value other than `true` is the person's own message.
- `image_ids` — optional; the ids the `POST /api/images` HTTP upload returned (see
  `docs/arch/subsystems/vision-documents.md`), at most `[vision] max_images` (default 5).
  **The only way to attach an image:** the daemon reads the stored files, sends them to the
  model, persists the turn as `text` + `[IMAGE:<id>]` markers, and echoes
  `server_saved: true` on the transcript so the client does **not** save the user row.
  Omit for a text-only turn.
- The turn is sent with every image it names or not at all. It fails with an `error` frame,
  and nothing is added to the conversation, when an id is malformed, names no image of the
  user's (deleted, another user's, unreadable), or there are too many:
  `IMAGE_UNAVAILABLE` (bad/missing/foreign id), `IMAGE_LIMIT` (more than `max_images`, or past
  the per-message size ceiling), `IMAGE_ERROR` (the server couldn't build the message).
  An inline `[IMAGE:data:...]` marker in `text` on an image turn also fails it
  (`IMAGE_UNAVAILABLE`): images come only from `image_ids`.
- `images` (base64 `[{data, mime_type}]`) is **no longer read** (since 2026-10): a frame that
  still carries it is processed as if it didn't; its images reach the model only through
  `image_ids`.
- `text` may be empty (or absent, or only whitespace) when `image_ids` has at least one id: the
  turn is just the images (flag `image_only_turns`), or when `attachments` has at least one
  document (the turn is the documents). With no words, images or attachments, the turn is
  refused with `EMPTY_MESSAGE`. The sender's own `transcript` echo carries the text the daemon built (the words, after any
  documents), never `[IMAGE:]` markers: for an image-only turn it is empty and the client shows
  the images it attached. Other
  viewers and a reload get the images from the saved row.
- A refused turn gets exactly one `error` frame: `TURN_QUEUE_FULL` when too many messages are
  already queued for the session, `EMPTY_MESSAGE`, `ATTACHMENT_INVALID`, an `IMAGE_*` code,
  `EMAIL_REF_LIMIT` / `EMAIL_UNAVAILABLE` (see Email, Email in chat), or `PROCESSING_ERROR` for
  anything else.
- Requires authentication

#### `cancel`
Cancel the current LLM operation for this session.
```json
{"type": "cancel"}
```
A turn still waiting for another turn on its conversation (`turn_waiting`, flag `turn_wait`)
stops waiting: it ends with `turn_unsent` and nothing of it is saved.

#### `stop_other_reply`
Stop the turn this session's turn waits for (flag `turn_wait`): the one holding the
conversation, run by another session of the same user (another tab or app, a background job, a
messaging channel).
```json
{"type": "stop_other_reply", "payload": {"conversation_id": 123}}
```
Answered with `stop_other_reply_response` once the stop is asked for, not when that turn ends:
```json
{"type": "stop_other_reply_response",
 "payload": {"conversation_id": 123, "success": true}}
```
- `error` (with `success: false`): `NOT_RUNNING` (no turn of the user's holds the conversation,
  including another user's, which answers the same, or the turn ended before the stop reached
  it) or `FORBIDDEN` (the turn holding it is this session's own: use `cancel`).
- A background job's own turn is cancelled as the job (recorded as cancelled); any other turn
  is stopped as `cancel` stops it. A turn inside a tool call stops when the call returns; the
  waiting turn then starts.

#### `reconnect`
Reconnect to an existing session using a stored token.
```json
{
   "type": "reconnect",
   "payload": {
      "token": "a1b2c3d4...",
      "audio_codecs": ["opus", "pcm"],
      "tts_enabled": true,
      "tool_step_origin": true,
      "session_keepalive": false
   }
}
```
- If token is valid, session is restored with conversation history
- If token is invalid/expired, a new session is created. A session no client is attached to
  may also be ended early, when every session slot is in use and another client needs one
- `audio_codecs` is optional, used to detect Opus support
- `tts_enabled` (optional, default false): server synthesizes and streams TTS audio to this connection
- `tool_step_origin` (optional, default false): a **client capability** — set it if this client
  renders its own turn's tool steps from the `tool_step` frame (uniform "tool pill" UI) rather than
  from its live text stream. When set, the server includes this connection in its **own** turn's
  `tool_step` fan (see `tool_step` under Server → Client). Default off preserves the origin-excluded
  behavior for clients that render tool steps inline from the stream. **The same fields are
  accepted on the initial connect handshake**, not only on `reconnect`.
- `client` (optional): who the client is, `{name, version, protocol}` (see Protocol
  version and feature flags). Logged; accepted on `init` too.
- `session_keepalive` (optional, default false): a per-browser **"always on"** hint that this
  client has session-keepalive enabled. **Hint only** — it is NOT the authorization to extend the
  session (an attacker holding a token could otherwise set it). The authoritative state is the
  server-persisted `keepalive_enabled` flag, set only by the authenticated `session_keepalive_enable`
  message (see Session Management). When that flag is set, the authenticated heartbeat `ping` moves
  this session's expiry on as use does (at most once a day, up to a 30-day absolute cap from session
  creation).
- **Single-connection-per-session:** if the target session is already held by another
  live connection, that connection is **evicted** — it receives a `session_superseded`
  frame + a `4001` close (see below). Last deliberate reconnect wins; the reconnecting
  client gets `reconnected:true` on its `session` frame.

#### `capabilities_update`
Update client capabilities after initial connection.
```json
{
   "type": "capabilities_update",
   "payload": {
      "audio_codecs": ["opus"]
   }
}
```

---

#### `attach_conversation`
Like `load_conversation`, but also replays the durable event log — the entry point
for any observe client. `last_seq` is an **exclusive** cursor: `0` replays
everything; on reconnect pass the highest `seq` already seen to receive only the gap.
```json
{ "type": "attach_conversation", "payload": { "conversation_id": 1006, "last_seq": 0 } }
```

#### `jobs_request`
Ask for the caller's complete **active** job set. Sent on connect/reconnect.
Response: `jobs_snapshot`.
```json
{ "type": "jobs_request" }
```

#### `list_jobs`
Ask for a page of the caller's **terminal** jobs (history). Omit the cursor for the
first page, then echo back the previous response's `next_before_*`. `limit` is
clamped server-side (default 25, max 50). Response: `list_jobs_response`.
```json
{
   "type": "list_jobs",
   "payload": { "before_created_at": 1784949100, "before_id": 998, "limit": 25 }
}
```

#### `job_action`
Cancel, resume or delete one background job. Ownership is checked against the connection's
authenticated user **before** acting; a job belonging to someone else and a job
that does not exist give the same answer, so this cannot be used to probe for
other users' job ids. Always answered with `job_action_response`.
```json
{ "type": "job_action", "payload": { "action": "cancel", "conversation_id": 1009 } }
```
- `cancel` — signals a running job, or retires a still-queued one.
- `resume` — re-dispatches an `interrupted` or `failed` job, continuing its
  existing transcript. Counts as a spawn against the job caps. Refused for
  `done` (it has an answer), `cancelled` (stopped deliberately), and `running`.
- `delete` — deletes a finished job's conversation, as `delete_conversation` does
  (its child jobs and images go with it), then sends `jobs_invalidate`. Refused while
  the job is queued or running, and for a coding job whose task the agent gateway may
  still hold (its trail has gateway events and none saying the task was closed): close
  it first. `delete_conversation` refuses the same two.

#### `coding_pending`
What coding jobs wait on (built with the coding agent only). Asks the agent gateway,
so the answer, `coding_pending_response`, comes a little later and goes to every tab
of the user. Jobs that aren't the user's coding jobs are left out of it. At most 16 ids.
One lookup per user runs at a time; asks that arrive meanwhile are merged into the next.
```json
{ "type": "coding_pending", "payload": { "jobs": [1977, 1983] } }
```

#### `coding_view`
The changes viewer: a coding job's work, read from the agent gateway's mirror (never
shown to the model). `what` is `changes` (the changed files and commits) or `file`
(one file: `path` exactly as `changes` listed it, `"old -> new"` for a rename;
`view` `diff`, the default, or `new`). `scope` (optional, either `what`) narrows the
view: `all` (the default, the whole job), `since_push` (after the last published head)
or one of the job's commits by hash (4-40 lowercase hex digits; against its first
parent, and for a merge only its files with conflicts); view only, Publish and Close
still act on the whole job. The user's own coding job only. One request
per user at a time; a newer one replaces one still waiting. Answered with
`coding_view_response`, to the asking tab only; `nonce` (optional, up to 39
characters) is echoed so the tab renders only its latest.
```json
{ "type": "coding_view", "payload": { "job": 42, "what": "file", "path": "src/a.c", "view": "diff", "nonce": "v3" } }
```

#### `coding_publish_preview`
The changes viewer's **Publish…**: DAWN's own preview of what publishing the coding job
would push and open, built with the same checks as the coding agent tool's publish. The
user's own coding job only. Answered with `coding_publish_preview_response`, to the
asking tab only; `nonce` (up to 39 characters) is echoed.
```json
{ "type": "coding_publish_preview", "payload": { "job": 42, "nonce": "a1" } }
```

#### `coding_publish`
Publish a coding job from the preview the user saw: `head` (the preview's full commit
hash) and `pr_body_sha256` (the preview's) bind the click to that preview; the gateway
refuses when the branch moved or the description changed since. The user's own coding
job, finished (`done`). The title is the agent's, else the job's name (set by the server).
Optional `pr_title` and `pr_description` are the user's own text for a new pull request,
sent instead of the agent's: the title as one line (up to 120 bytes), the description up
to 5900 bytes with the preview's `pr_footer` appended by the gateway; with
`pr_description`, `pr_body_sha256` may be left out. The user's own text is refused when
the job already has an open pull request (its text is edited on GitHub); its new commits
still go there. On success DAWN
posts its line to the job's parent conversation and drops any publish staged in chat for
that job. Answered with `coding_publish_response`, to the asking tab only.
```json
{ "type": "coding_publish", "payload": { "job": 42, "head": "4f1c…(40 hex)", "pr_body_sha256": "9a0b…", "nonce": "a2" } }
```

#### `coding_close`
Close a coding job (its clone and branch go; it can't be revised afterwards). Without
`confirm`: closes when nothing would be lost, and otherwise answers with
`needs_confirm`. With `confirm: true` ("Close anyway"): closes with `head`, the head the
`needs_confirm` answer showed (`""` when only uncommitted edits would be lost); refused
when the work changed since. The `confirm` key, not an empty head, tells the two apart.
Answered with `coding_close_response`, to the asking tab only.
```json
{ "type": "coding_close", "payload": { "job": 42, "nonce": "a3", "confirm": true, "head": "" } }
```

#### `coding_grants`
The user's standing approvals for the coding agent (hosts and tools allowed for every
task on a repo, or every repo). Answered with `coding_grants_response`.
```json
{ "type": "coding_grants", "payload": {} }
```

#### `coding_grant_revoke`
Remove one standing approval; the gateway removes only the user's own. Answered with
`coding_grants_response` carrying the result and the list as it now stands.
```json
{ "type": "coding_grant_revoke", "payload": { "id": 3 } }
```

#### `coding_reply`
The user's answer to one open request of a coding job, from a card. `decision` is
`allow` (this task), `allow_repo` (every task on the job's repo), `deny`, or `answer`
(with `answer`, the user's words, at most 2000 characters, for a question). For
`allow`/`allow_repo` send `line`, the line the card showed. The server checks the
click against the live request: the job is the user's coding job and the request is
still open; Allow only when DAWN can show what it asks and `line` still matches;
`allow_repo` only when the repo can be named; answers only for questions (at most
2000 bytes), decisions only for hosts and tools (`deny` on a question declines it).
One reply per job at a time. `nonce` (optional, up to 39 characters) is echoed back,
so the tab that clicked can tell its answer from another tab's. Answered with
`coding_reply_response`, to every tab of the user.
```json
{ "type": "coding_reply", "payload": { "job": 1977, "request_id": 12, "decision": "allow", "line": "reach the host `downloads.arduino.cc`" } }
```

### Configuration (Admin Only)

#### `get_config`
Request the full daemon configuration.
```json
{"type": "get_config"}
```
Response: `get_config_response`. The payload includes an `llm_runtime` object with the
session's *resolved/actual* LLM state (not just config defaults):
```json
{
   "llm_runtime": {
      "type": "cloud",
      "provider": "Claude",
      "model": "claude-...",
      "openai_available": true,
      "claude_available": true,
      "gemini_available": false,
      "context_max": 200000,
      "thinking_mode": "enabled",
      "reasoning_effort": "medium"
   }
}
```
- `thinking_mode` / `reasoning_effort`: the session's current reasoning settings (same
  vocabulary as `set_session_llm`). This is the only place a fresh connection learns
  them — `llm_state_update` is only pushed on a `switch_llm` tool call.

#### `set_config`
Update daemon configuration settings.
```json
{
   "type": "set_config",
   "payload": {
      "section.key": "value"
   }
}
```
Response: `set_config_response`

#### `set_secrets`
Update API keys and credentials.
```json
{
   "type": "set_secrets",
   "payload": {
      "openai_api_key": "sk-...",
      "claude_api_key": "sk-ant-..."
   }
}
```
Response: `set_secrets_response`

#### `restart`
Request daemon restart. **Admin only.**
```json
{"type": "restart"}
```
Response: `restart_response`

---

### Home Assistant and Phone Audio (Admin Only)

The admin panels for the Home Assistant connection and entity board, and the phone's call audio.
**Admin only**: other users get an `error` frame `FORBIDDEN` (`UNAUTHORIZED` without a login). No
payload, except `ha_call_service`.
Built with the Home Assistant tool (`DAWN_ENABLE_HOMEASSISTANT_TOOL`) and the phone tool
(`DAWN_ENABLE_PHONE_TOOL`); a daemon without the tool answers its type with `UNKNOWN_TYPE`.
Both settings are saved with `set_config`, in a `home_assistant` or `phone` section, and that
save is also answered with the frame below.

#### `ha_status`
```json
{ "type": "ha_status" }
```
Response:
```json
{
   "type": "ha_status_response",
   "payload": {
      "configured": true, "connected": true,
      "entity_count": 42, "version": "2026.11.0", "url": "http://192.0.2.10:8123",
      "led_hue_correction": 0
   }
}
```
- `configured`: the Home Assistant service is set up (URL and token). `connected`: DAWN
  currently reaches it.
- `entity_count`, `version`, `url`: always present (`version` and `url` are `""` when unknown).
- `led_hue_correction`: the configured hue offset for LED lights, 0–60 degrees.
- No `success` member. Also sent after a `set_config` that changes the `home_assistant` section
  (`url`, `enabled`, `led_hue_correction`).

#### `ha_test_connection`
Call Home Assistant now.
```json
{ "type": "ha_test_connection" }
```
Response `ha_test_connection_response`, whose payload is `{"success": true, "version": "2026.11.0"}`,
or `{"success": false, "error": "…"}` with
"Home Assistant not configured", "Network error", "API error", "Rate limited" or "Memory
allocation failed". The reply waits for Home Assistant's answer.

#### `ha_list_entities` / `ha_refresh_entities`
The Home Assistant entities DAWN knows, for the admin's entity board. No payload.
`ha_list_entities` reads DAWN's cache, which is refetched first when it is older than 5 minutes (that
reply then waits for Home Assistant too); `ha_refresh_entities` first fetches every entity from
Home Assistant again (the reply waits for it).
```json
{ "type": "ha_list_entities" }
```
Both are answered with `ha_entities_response`:
```json
{
   "type": "ha_entities_response",
   "payload": {
      "success": true,
      "count": 2,
      "cached_at": 1785000000,
      "entities": [
         { "entity_id": "light.office", "friendly_name": "Office Light", "domain": "light",
           "state": "on", "area": "Office", "attributes": { "brightness": 180 } },
         { "entity_id": "lock.front_door", "friendly_name": "Front Door", "domain": "lock",
           "state": "locked" }
      ]
   }
}
```
- At most 512 entities. `cached_at`: epoch seconds of the cache the list was read from.
- `area`: present only when the entity has a Home Assistant area.
- `attributes`: present only for the domains below; an entity without it is a plain on/off
  toggle or a display row.

| `domain` | `attributes` |
|---|---|
| `light` | `brightness` (0–255) |
| `fan` | `percentage` |
| `cover` | `current_position` |
| `climate` | `hvac_mode` (when known), `hvac_modes` (array, when known), `current_temperature`, `temperature` (the target) |
| `sensor`, `binary_sensor` | `unit_of_measurement`, `device_class` (each only when set; no `attributes` when neither is) |

- On failure `success: false` with `error`: "Not connected" (DAWN can't reach Home Assistant),
  "Home Assistant not configured", "Home Assistant not connected", "Network error", "API
  error", "Rate limited", "Memory allocation failed" or "Out of memory".
- The same frame also arrives unasked after a successful `ha_call_service`, to every browser
  connection of the admin who called it, while the realtime stream (`ha_state_changed`) isn't
  connected. With the stream connected the change arrives as `ha_state_changed` instead.

#### `ha_call_service`
Call a Home Assistant service on one entity: the entity board's controls.
```json
{
   "type": "ha_call_service",
   "payload": {
      "entity_id": "light.office",
      "service": "turn_on",
      "data": { "brightness": 128 }
   }
}
```
- `entity_id` (required): lowercase letters, digits, `_` and a `.`, under 128 bytes.
- `service` (required).
- `domain` (optional): the service's domain; absent or `""` takes the part of `entity_id`
  before the `.`.
- `data` (optional object): the service's parameters, sent to Home Assistant with `entity_id`
  added. A `data` that isn't an object is ignored.
- Only these `domain` / `service` pairs are allowed; any other is refused with "Service not
  permitted" (the model's Home Assistant tool isn't limited by this list):

| `domain` | `service` |
|---|---|
| `light`, `switch`, `input_boolean` | `turn_on`, `turn_off` |
| `fan` | `turn_on`, `turn_off`, `set_percentage` |
| `lock` | `lock`, `unlock` |
| `cover` | `open_cover`, `close_cover`, `set_cover_position` |
| `climate` | `set_hvac_mode`, `set_temperature` |
| `media_player` | `media_play`, `media_pause` |

Response, after Home Assistant has answered:
```json
{
   "type": "ha_call_service_response",
   "payload": { "success": true, "entity_id": "light.office", "error": null }
}
```
- On failure `success: false` with `error`: "entity_id and service are required" (no
  `entity_id` echoed then), "Not connected", "Service not permitted", "Invalid service data",
  "Invalid parameter" (a malformed `entity_id`, domain or service), "Network error", "API
  error", "Rate limited" or "Memory allocation failed".
- A success is followed by the entity's new state: `ha_state_changed` while the realtime stream
  is connected, else an unasked `ha_entities_response` (above).

#### `get_phone_audio_config`
The call-audio settings of the cellular modem bridge.
```json
{ "type": "get_phone_audio_config" }
```
Response (note the name: not `get_phone_audio_config_response`):
```json
{
   "type": "phone_audio_config_response",
   "payload": {
      "webrtc_available": true,
      "bridge_active": false,
      "pcm_port": "",
      "pcm_port_default": "/dev/ttyUSB4",
      "uplink": { "agc": true, "fixed_gain_db": 1.0, "agc_ramp_db_per_s": 3.0,
                  "max_output_noise_dbfs": -50.0, "ns_level": "moderate", "high_pass": true,
                  "echo_cancel": true },
      "downlink": { "agc": false, "fixed_gain_db": 1.0, "agc_ramp_db_per_s": 3.0,
                    "max_output_noise_dbfs": -50.0, "ns_level": "moderate", "high_pass": true },
      "downlink_apm": false,
      "downlink_gain": 0.0
   }
}
```
- `webrtc_available`: the daemon was built with WebRTC audio processing; without it every
  `uplink` / `downlink` control has no effect.
- `bridge_active`: a call's audio is running now (a change applies to it live).
- `pcm_port`: the modem audio device set in `dawn.toml`, `""` when the default
  (`pcm_port_default`) is used.
- `uplink` (the caller hears) / `downlink` (from the caller): `fixed_gain_db` 0–49,
  `agc_ramp_db_per_s` 0.1–100, `max_output_noise_dbfs` −90–0, `ns_level` `off` / `low` /
  `moderate` / `high` / `veryhigh`. `echo_cancel` is uplink only.
- `downlink_apm`: processing on the downlink; the downlink's `agc` always follows it.
  `downlink_gain`: a fixed gain on the downlink (0 = the bridge's default, at most 32).

A `set_config` with a `phone` section (any of the members above except `webrtc_available`,
`bridge_active` and `pcm_port_default`; absent members keep their value) is answered with this
frame too, holding the stored values after clamping. A `pcm_port` that isn't `/dev/ttyUSB<n>`,
`/dev/ttyACM<n>` or a `/dev/serial/by-id` / `by-path` alias, or is 128 bytes or longer, is
refused: nothing in the section is applied, and the frame carries the stored values with an
`error` sentence.

---

### Audio & Model Discovery

#### `get_audio_devices`
List available audio capture and playback devices.
```json
{
   "type": "get_audio_devices",
   "payload": {
      "backend": "alsa"
   }
}
```
Response: `get_audio_devices_response`

#### `list_models`
List available ASR and TTS models.
```json
{"type": "list_models"}
```
Response: `list_models_response`

#### `list_interfaces`
List available network interfaces.
```json
{"type": "list_interfaces"}
```
Response: `list_interfaces_response`

#### `list_llm_models`
List available local LLM models (from Ollama or llama.cpp).
```json
{"type": "list_llm_models"}
```
Response: `list_llm_models_response`

---

### LLM Runtime Control

#### `set_session_llm`
Configure LLM settings for this session only (does not affect other clients).
```json
{
   "type": "set_session_llm",
   "payload": {
      "type": "cloud",
      "provider": "openai",
      "model": "gpt-5-mini",
      "thinking_mode": "enabled",
      "reasoning_effort": "medium"
   }
}
```
- All fields are optional, only provided fields are changed
- Send a pick as one message: `type`, `provider` (for cloud) and `model` together
- `type`: `"local"`, `"cloud"`, or `"reset"`: no selection of its own, so the session
  follows the dawn.toml default (resolved at each turn) with the default reasoning; other
  fields in the same message apply over it.  `"cloud"` without `provider` means the
  session's cloud provider, else the one dawn.toml names
- `provider`: `"openai"`, `"claude"`, `"gemini"`, `"openrouter"`; `"local"` means local;
  `"none"` (what the server sends for a local selection) names none.  A changed provider
  clears the model (its default), so one vendor's model never goes to another
- `model` alone (no `type` or `provider`) moves the session to cloud only when a dawn.toml
  cloud model list names it exactly; otherwise it stays where it is
- A pick that can't run (its provider has no API key, an unknown provider) is refused:
  `success` false, `error` the reason, nothing stored, and the payload carries the session's
  current settings to resync from.  A restore (`from_restore`) or a `"reset"` is checked at
  its next turn
- `from_restore`: true when the client replays a loaded conversation's settings; the server
  then doesn't save them back to the active conversation
- The response, `llm_runtime` in `get_config_response` and a loaded conversation's
  `llm_settings` carry `follows_default` (true: no selection of its own; the values shown
  are the default's). A client restores such a conversation with `"reset"`.
- `thinking_mode`: `"disabled"`, `"auto"`, or `"enabled"`
- `reasoning_effort`: `"low"`, `"medium"`, or `"high"`
- `type: "local"` with no `model` clears a model the session carried over from a cloud
  session, or a bare cloud model name. The session then runs the configured local model
  until one is named.
- When setting a local model, send `type: "local"` with `model`. Without the type, a name
  that starts like a cloud model's (Ollama's `gpt-oss`) is taken for a cloud model and the
  session moves to cloud.
- `from_restore`: `true` applies the settings to the session without saving them to the
  active conversation (a loaded conversation's own settings, or a model the client
  detected rather than the user picked).
- `conversation_id`: optional; the conversation the change was made in (`0`: one not
  saved yet). When it isn't the connection's active conversation, nothing in the request is
  applied or saved (the session now carries that conversation's settings): the reply is
  `success` false with the current settings.
- Response: `set_session_llm_response`

#### `get_system_prompt`
Request the current system prompt for debugging.
```json
{"type": "get_system_prompt"}
```
Response: `system_prompt_response`

---

### Tools Configuration

#### `get_tools_config`
Get the current tool enable/disable configuration.
```json
{"type": "get_tools_config"}
```
Response: `get_tools_config_response`

#### `set_tools_config`
Update tool enable/disable settings.
```json
{
   "type": "set_tools_config",
   "payload": { ... }
}
```
Response: `set_tools_config_response`

---

### Metrics

#### `get_metrics`
Request current system metrics (uptime, sessions, etc.).
```json
{"type": "get_metrics"}
```
Response: `get_metrics_response`

---

### TTS Control

#### `set_tts_enabled`
Enable/disable TTS audio for this connection.
```json
{
   "type": "set_tts_enabled",
   "payload": {
      "enabled": true
   }
}
```
No response. Requires authentication.

---

### Always-On Voice (Authenticated)

Always-on mode: the browser streams its microphone continuously and the server listens for the
wake word itself (VAD and wake word on the server, no wake-word model in the browser). Audio is
sent as ordinary `AUDIO_IN` (`0x01`) binary frames, at most 150,000 bytes a second per
connection (frames past that are dropped). Each request below is answered with an
`always_on_state` frame (below) or an `error` frame. One always-on connection per browser login:
another machine logged in as the same user has its own.

#### `always_on_enable`
```json
{"type": "always_on_enable", "payload": {"sample_rate": 48000}}
```
- `sample_rate` (optional, default 48000): the capture rate; one of 8000, 16000, 22050, 44100,
  48000, else `INVALID_SAMPLE_RATE`.
- Refused with `PTT_ACTIVE` while a push-to-talk recording is in progress on the connection,
  `ALREADY_ACTIVE` when another connection of the same browser login has it on, `INIT_FAILED`
  when it can't be started.
- Already on for this connection: answered with `always_on_state` `listening` again.

On success: `always_on_state` `listening`.

#### `always_on_disable`
```json
{"type": "always_on_disable"}
```
Answered with `always_on_state` `disabled` (also when it was already off).

#### `always_on_state_request`
Ask for the current state again (a tab coming back from the background).
```json
{"type": "always_on_state_request"}
```
Answered with `always_on_state`; `disabled` when always-on isn't on for this connection.

---

### User Management (Admin Only)

#### `list_users`
List all users.
```json
{"type": "list_users"}
```
Response: `list_users_response`

#### `create_user`
Create a new user account.
```json
{
   "type": "create_user",
   "payload": {
      "username": "alice",
      "password": "secret123",
      "is_admin": false
   }
}
```
Response: `create_user_response`

#### `delete_user`
Delete a user account.
```json
{
   "type": "delete_user",
   "payload": {
      "username": "alice"
   }
}
```
Response: `delete_user_response`

#### `change_password`
Change a password. Any logged-in user may change **their own**; an admin may change anyone's.
```json
{
   "type": "change_password",
   "payload": {
      "username": "alice",
      "current_password": "oldpass123",
      "new_password": "newpass123"
   }
}
```
- `username` and `new_password` are required. `new_password` must be at least 8 characters.
- A non-admin may name only their own `username` (else `"Cannot change another user's
  password"`), and must send `current_password` (`"Current password required"` /
  `"Current password incorrect"`). An admin needs no `current_password`, for their own
  password or another user's.
- Changing your own password ends every other login of yours and keeps this one. Changing
  another user's ends all of theirs (their connections get `force_logout`).

Response: `change_password_response`, with `success`, and `message` (`"Password changed"`) or
`error` (also `"User not found"`).

#### `unlock_user`
Unlock a locked-out user account.
```json
{
   "type": "unlock_user",
   "payload": {
      "username": "alice"
   }
}
```
Response: `unlock_user_response`

---

### Satellite Administration (Admin Only)

Every request here is **admin only** (`FORBIDDEN` otherwise). Failures come back as an
`error` frame (`INVALID_PARAM`, `NOT_FOUND`, `DB_ERROR`, `FORBIDDEN`, `OTA_ERROR`), not as a
`success: false` response. The device side of OTA (`ota_offer`, `ota_ack`, `ota_reject`,
`ota_status`) is under DAP2 Satellite Messages, OTA.

#### `list_satellites`
List every registered satellite, the users it can be assigned to, and the Home Assistant
areas.
```json
{"type": "list_satellites"}
```
Response `list_satellites_response`:
```json
{
   "type": "list_satellites_response",
   "payload": {
      "satellites": [
         {
            "uuid": "11111111-2222-3333-4444-555555555555",
            "name": "Kitchen", "location": "Kitchen", "ha_area": "Kitchen",
            "user_id": 3, "tier": 1, "enabled": true,
            "last_seen": 1795000000, "created_at": 1790000000, "online": true,
            "firmware_version": "2.0.0", "ota_state": "idle", "ota_target_version": ""
         }
      ],
      "users": [ { "id": 3, "display_name": "alice" } ],
      "ha_areas": [ "Kitchen", "Office" ]
   }
}
```
- At most 64 satellites are listed.
- `user_id`: `0` when the satellite is assigned to no one. `tier` `0` is the **Local Device**
  (uuid `00000000-0000-0000-0000-000000000000`): the daemon's own microphone and speaker.
- `firmware_version` / `ota_state` / `ota_target_version`: what the device last reported, and
  any update in flight. `ota_state` is one of `idle`, `offered`, `downloading`, `verifying`,
  `applying`, `rebooting`, `success`, `failed`, `unknown`; a device with no OTA record shows
  `""` / `"idle"` / `""`.
- `users[].display_name` is the username.
- `ha_areas` is present only while Home Assistant is configured and reports areas.

#### `update_satellite`
Assign a satellite to a user and/or set its Home Assistant area.
```json
{
   "type": "update_satellite",
   "payload": {
      "uuid": "11111111-2222-3333-4444-555555555555",
      "user_id": 3,
      "ha_area": "Kitchen"
   }
}
```
- `uuid` (required): 36 characters, else `INVALID_PARAM`. Unknown → `NOT_FOUND`.
- `user_id` (optional): the user to assign; `0` or less unassigns. A user that can't be
  assigned → `DB_ERROR` "Could not assign that user". Assigning the Local Device also moves
  the daemon's own microphone to that user (between local turns).
- `ha_area` (optional): any character other than letters, digits, space, `-` and `_` is
  replaced by `_`. The satellite's `location` is unchanged.
- The satellite is then disconnected so it reconnects with the new settings (even when nothing
  changed).

Response `update_satellite_response`: `{success: true, satellite: {…}}`, the satellite object
as `list_satellites` gives it, with `online: false` (it is reconnecting), except that `user_id`
echoes the request's value (a negative one is stored as unassigned but echoed as sent).

#### `delete_satellite`
Remove a satellite's registration. It is disconnected first.
```json
{"type": "delete_satellite", "payload": {"uuid": "11111111-2222-3333-4444-555555555555"}}
```
The Local Device can't be deleted (`FORBIDDEN`; reassign it instead). Unknown uuid →
`NOT_FOUND`. Response `delete_satellite_response`: `{success: true, uuid}`.

#### `get_satellite_registration_key`
Fetch the satellite registration key, to show it as a pairing QR code.
```json
{"type": "get_satellite_registration_key"}
```
Response type **`satellite_registration_key_response`** (not `get_…_response`):
```json
{
   "type": "satellite_registration_key_response",
   "payload": { "key_set": true, "key": "<64 hex characters>", "tls_enabled": true }
}
```
- `key` is `""` when `key_set` is false.
- `tls_enabled`: the WebUI serves HTTPS; a client must not build a `ws://` pairing payload
  then.
- Limited to 5 fetches a minute and 30 an hour per source address (IPv6 per /64): past it,
  an `error` frame with code `rate_limited`. A configured key that isn't exactly 64
  characters gets an `error` frame with code `malformed_key`. (These two codes are
  lowercase.)
- Every fetch is audit-logged; the key itself never is.

#### `ota_list`
The releases the daemon can offer.
```json
{"type": "ota_list"}
```
Response `ota_list_response`:
```json
{
   "type": "ota_list_response",
   "payload": {
      "enabled": true,
      "releases": [ { "platform": "rpi", "tier": 1, "version": "2.1.0" } ]
   }
}
```
`enabled`: whether `[ota]` is on; with it off every push fails.

#### `ota_push`
Offer one release to one device.
```json
{
   "type": "ota_push",
   "payload": {
      "uuid": "11111111-2222-3333-4444-555555555555",
      "version": "2.1.0",
      "allow_downgrade": false
   }
}
```
- `uuid` and `version` required: absent → `INVALID_PARAM`; present but empty → `OTA_ERROR`. `allow_downgrade` defaults to false and is
  passed to the device in the offer.
- The release's platform follows the device's tier (tier 2 → `esp32`, else `rpi`).
- The device must be online and must have advertised `capabilities.ota` at registration.
- Failures are an `error` frame, code `OTA_ERROR`, with message `"device is already
  mid-update"`, `"device is offline"`, or `"no matching release, or push failed"` (also:
  unknown device, no OTA capability, OTA off).

Response `ota_push_response`: `{success: true, uuid, version}`. Progress then shows in the
device's `ota_state` (`list_satellites`).

#### `ota_push_all`
Start a fleet rollout of one release to every eligible device of a platform: one canary first,
then the rest once the canary comes back on the new version.
```json
{
   "type": "ota_push_all",
   "payload": { "platform": "rpi", "version": "2.1.0", "allow_downgrade": false }
}
```
- `platform` (`"rpi"` or `"esp32"`) and `version` required (`INVALID_PARAM`).
- Eligible: online devices of that tier not already on `version`, at most 64 (the rest are
  counted as skipped).
- One rollout at a time. The canary has 10 minutes to report the new version.
- A rollout that can't start is an `error` frame, code `OTA_ERROR`, whose message says why
  (`"A rollout is already in progress (abort it first)."`, `"No eligible devices (none online,
  or all already on 2.1.0)."`, the canary couldn't be offered, …).

Response `ota_push_all_response`: `{success: true, summary}`, `summary` a sentence such as
`"Rollout started: canary <uuid> for 2.1.0, 3 device(s) queued, 1 skipped."`

#### `ota_rollout_status`
```json
{"type": "ota_rollout_status"}
```
Response `ota_rollout_status_response`: `{status}`, a sentence for people, not for parsing:
`"No rollout has run."`, `"Rollout starting…"`, or `"Rollout rpi/2.1.0 — rolling out (tier 1):
4 target(s), 1 committed, 0 failed, 1 skipped. Canary: <uuid>."` (state is one of `waiting on
canary`, `rolling out`, `done`, `failed`; a ` Last error: …` may follow). No `success` field.

#### `ota_rollout_abort`
Stop the rollout in progress (offers already made aren't withdrawn).
```json
{"type": "ota_rollout_abort"}
```
Response `ota_rollout_abort_response`: `{aborted}`; `false` when no rollout was running.

---

### Personal Settings (Authenticated)

#### `get_my_settings`
Get the current user's personal settings.
```json
{"type": "get_my_settings"}
```
Response: `get_my_settings_response`, with `success` and (on failure) `error`, and:

| Field | Meaning |
|---|---|
| `base_values`, `base_voice` | The server's half of the persona that the user's text builds on (`[persona] values` / `voice`, else the built-in; while `[persona] description` is set it is `base_values` and `base_voice` is empty) |
| `persona_values`, `persona_voice` | The user's text for each half (`""` when unset) |
| `persona_values_mode`, `persona_voice_mode` | `"append"` (added after the base) or `"replace"` (used instead of it) |
| `persona_description`, `persona_mode` | The older single persona text and its mode: `"replace"` replaces the whole persona, `"append"` is added to the user's context. Shown only while set |
| `location`, `timezone`, `server_timezone`, `units`, `theme` | `timezone` `""` is the server's zone (`server_timezone`) |
| `real_name`, `preferred_address`, `identity_aliases` | `preferred_address` is comma-separated forms; `identity_aliases` one per line |

#### `set_my_settings`
Update the current user's personal settings.  Any field above except `base_values`, `base_voice`
and `server_timezone`; a field left out keeps its value, a mode other than `"append"` or
`"replace"` is ignored, and a `timezone` DAWN doesn't know is refused.  `persona_description` is
sent to clear the older text.
```json
{
   "type": "set_my_settings",
   "payload": {
      "persona_voice": "Speak with a slight British accent.",
      "persona_voice_mode": "append",
      "timezone": "America/New_York"
   }
}
```
Response: `set_my_settings_response`, with `success`, and `message` or `error`

---

### Session Management (Authenticated)

#### `list_my_sessions`
List all active sessions for the current user.
```json
{"type": "list_my_sessions"}
```
Response: `list_my_sessions_response`

#### `revoke_session`
Revoke (terminate) a specific session.
```json
{
   "type": "revoke_session",
   "payload": {
      "token_prefix": "abc123def4567890"
   }
}
```
`token_prefix` is the 16-character prefix `list_my_sessions` returns; a shorter value is refused.

Response: `revoke_session_response`

#### `session_keepalive_enable`
Opt this browser's session into **keepalive** ("always on"). While the session actively heartbeats
(`ping`), the server moves its `expires_at` on as use does (to 7 days ahead, at most once a day)
instead of letting it end after 7 idle days — up to the **30-day absolute cap** measured from session
creation.
The decision is persisted as the `keepalive_enabled` flag on the session row (survives reconnects on
the **same** token; a **fresh** session starts with it off, so the client re-sends this on landing a
fresh session). This is the authorization gate — the `session_keepalive` connect-payload hint above
is advisory only.
```json
{"type": "session_keepalive_enable"}
```
Requires an authenticated session. No response frame; takes effect on the next heartbeat renewal.
The enable/disable events are audit-logged (`SESSION_KEEPALIVE_ENABLE`/`_DISABLE`).

#### `session_keepalive_disable`
Turn keepalive back off for this session: it then ends a week after its last use, like any other
login (see How long a login lasts), and the heartbeat no longer moves it.
```json
{"type": "session_keepalive_disable"}
```
Requires an authenticated session. No response frame.

---

### Messaging Channels (Authenticated)

Each user manages their own Telegram, Discord, Slack and SMS channels (a channel answers only
the person who linked it). Everything is scoped to the logged-in user, and operations key on the
channel's `id`, never its name. A malformed request (a missing or bad `id`, a value out of
range) is refused with an `error` frame (`INVALID_PARAM`, `NOT_FOUND`); an operation on a
channel answers in its own response type, failure included:
`{success: true, id}` or `{success: false, id, code, message}`, where `message` is a sentence
to show the user.

#### `list_channels`
```json
{"type": "list_channels"}
```
Response `list_channels_response`, most recently used first:
```json
{
   "type": "list_channels_response",
   "payload": {
      "channels": [
         {
            "id": 12, "name": "my phone", "provider": "sms",
            "enabled": true, "verified": false, "verify_ttl_seconds": 420,
            "last_used_at": 1795000000, "conversation_id": 0,
            "llm_type": "", "cloud_provider": "", "model": "",
            "thinking_mode": "", "reasoning_effort": ""
         }
      ]
   }
}
```
- `enabled`: `false` for an unlinked channel (`reenable_channel` brings it back).
- `verified`: `false` for an SMS link still waiting for the code texted to the number; then
  `verify_ttl_seconds` is the code's remaining life (`0` expired, `-1` none sent). Absent when
  verified.
- `conversation_id`: the channel's conversation, `0` until its first message. The `llm_*`,
  `model`, `thinking_mode`, `reasoning_effort` fields are that conversation's model settings;
  `""` follows the default.
- No `success` field: a failure gives an empty `channels` array.

#### `create_link_code`
Make a code to send from the chat (`/link CODE`) to link it to this account.
```json
{"type": "create_link_code", "payload": {"provider": "telegram"}}
```
- `provider` (optional): `telegram`, `discord`, `slack` or `sms`; another value is
  `INVALID_PARAM`. Omitted or `""`: the code works from any provider.
- A code is 8 characters and lasts 10 minutes. A user may hold at most 5 unused codes: past
  that, an `error` frame `RATE_LIMITED`. Any other failure: `SERVICE_ERROR`.

Response `create_link_code_response`: `{code, ttl_seconds: 600, provider}` (`provider` `""`
when none was given).

When the chat sends the code, the server pushes `messaging_channels_changed` with `change`
`linked` (or `pending` for an SMS number, which still needs the code DAWN texts back) and the
code that was used as `link_code`.

#### `unlink_channel`
```json
{"type": "unlink_channel", "payload": {"id": 12}}
```
Response `unlink_channel_response`. `code`: `NOT_FOUND` (already unlinked), `SERVICE_ERROR`.
The channel stays in the list with `enabled: false`.

#### `reenable_channel`
Undo an unlink.
```json
{"type": "reenable_channel", "payload": {"id": 12}}
```
Response `reenable_channel_response`. `code`: `NOT_FOUND` (it isn't unlinked), `NAME_TAKEN`
(an active channel has its name), `ALREADY_LINKED` (the chat is linked to another DAWN account),
`NOT_VERIFIED` (an SMS number that must be linked again with a new code), `SERVICE_ERROR`.

#### `rename_channel`
```json
{"type": "rename_channel", "payload": {"id": 12, "name": "work phone"}}
```
- `name` required and non-empty (`INVALID_PARAM` error frame). Under 64 bytes, with no quotes,
  angle brackets, backslashes or control characters (`code` `INVALID_PARAM` in the response).

Response `rename_channel_response` (`{success, id}`; the new name isn't echoed). `code`:
`NOT_FOUND`, `NAME_TAKEN`, `INVALID_PARAM`, `SERVICE_ERROR`.

#### `verify_channel`
Finish an SMS link with the code DAWN texted to the number.
```json
{"type": "verify_channel", "payload": {"id": 12, "code": "123456"}}
```
Response `verify_channel_response`. `code`: `BAD_CODE` (missing, not a string, or wrong: a code
lasts 10 minutes and a number allows 10 tries a day), `NOT_FOUND` (the channel isn't waiting
for a code), `ALREADY_LINKED` (the number is linked to another account), `SERVICE_ERROR`.

#### `resend_channel_code`
Text a new code to an SMS channel waiting for one.
```json
{"type": "resend_channel_code", "payload": {"id": 12}}
```
Response `resend_channel_code_response`. `code`: `RATE_LIMITED` (3 codes a day per number, 5
per account), `UNAVAILABLE` (the phone service isn't running), `NOT_FOUND`, `SERVICE_ERROR`.

#### `set_channel_llm`
Set the model a channel's conversation runs on.
```json
{
   "type": "set_channel_llm",
   "payload": {
      "conversation_id": 345,
      "llm_type": "cloud",
      "cloud_provider": "claude",
      "model": "claude-example-model",
      "thinking_mode": "adaptive",
      "reasoning_effort": "medium"
   }
}
```
- `conversation_id` (required): the channel's `conversation_id` from `list_channels`. `0` (no
  message yet) is refused with `INVALID_PARAM`. It must be the user's and a messaging channel's
  conversation (`NOT_FOUND` / `INVALID_PARAM`).
- The other fields are optional; absent or `""` follows the default. `llm_type`: `local` |
  `cloud`; `cloud_provider`: `openai` | `claude` | `anthropic` | `gemini` | `openrouter`;
  `thinking_mode`: `disabled` | `adaptive` | `enabled` | `auto`; `reasoning_effort`: `none` |
  `minimal` | `low` | `medium` | `high` | `xhigh` | `max`. `model` up to 63 bytes, the others up
  to 15. A value outside these is an `INVALID_PARAM` error frame.
- A choice that can't run (a provider without its key) is not saved.

Response `set_channel_llm_response`: `{success, conversation_id, error?}`, `error` saying why it
wasn't saved. The channel's next message runs on the new settings.

#### `messaging_channels_changed` (push)
The user's channel list changed: a chat sent `/link`, a code was verified or sent, a channel
was unlinked, renamed or re-enabled (from any tab or `dawn-admin`), or unverified SMS links
expired. To every browser connection of the channel's owner. Re-read with `list_channels`.
```json
{
   "type": "messaging_channels_changed",
   "payload": { "channel_id": 12, "change": "pending", "link_code": "ABCD2345" }
}
```
- `change`: `linked`, `pending` (an SMS number waiting for its code), `code_sent`,
  `code_voided` (a code that couldn't be sent), `verified`, `unlinked`, `renamed`, `enabled`,
  `removed` (expired unverified SMS links; `channel_id` `0`).
- `link_code`: only with `linked` / `pending`, the link code the chat used, so a client showing
  that code can close it.

---

### Conversation History (Authenticated)

#### `list_conversations`
List saved conversations.
```json
{
   "type": "list_conversations",
   "payload": {
      "limit": 20,
      "offset": 0
   }
}
```
Response: `list_conversations_response`

#### `new_conversation`
Create a new conversation (clears current session context).
```json
{
   "type": "new_conversation",
   "payload": {
      "title": "New Title"
   }
}
```
Response: `new_conversation_response` — `{success, conversation_id, is_private}`. The
conversation is private from its first row when `set_private` with `conversation_id: 0` came
first. A voice turn that creates the conversation sends the same frame unsolicited, with
`server_initiated: true`.

#### `load_conversation`
Load a saved conversation into the current session.
```json
{
   "type": "load_conversation",
   "payload": {
      "conversation_id": 42
   }
}
```
Response: `load_conversation_response`

The response's `llm_settings.model` is `""` when the conversation stores no model and is
not on the default type and provider: it runs that type's or provider's own default.

A user row that asked about an attached email carries `email_ref` (an object:
`account_id`, `message_id`, `from`, `subject`; see Email, Email in chat). Rows without one
have no such field.  An assistant row the user stopped before it finished carries
`stopped: true` (flag `stopped_replies_saved`); other rows have no such field.

#### `set_active_conversation`
Re-anchor the connection's active conversation **without** replaying history — a
lightweight alternative to `load_conversation` for the reconnect case, where the
client only needs to reset which conversation its turns persist into (not reload
the transcript).
```json
{
   "type": "set_active_conversation",
   "payload": {
      "conversation_id": 42
   }
}
```
Response: `set_active_conversation_response` — `{success, conversation_id, is_private}`
on success; `{success:false, conversation_id, error}` on failure. Ownership-checked
(owner-scoped, non-oracle): a conversation the caller does not own returns
`error: "Conversation unavailable"`, indistinguishable from an absent id. Does not
touch `stream_conversation_id`.

#### `delete_conversation`
Delete a saved conversation.
```json
{
   "type": "delete_conversation",
   "payload": {
      "conversation_id": 42
   }
}
```
Response: `delete_conversation_response`

#### `rename_conversation`
Rename a conversation.
```json
{
   "type": "rename_conversation",
   "payload": {
      "conversation_id": 42,
      "title": "New Title"
   }
}
```
Response: `rename_conversation_response`

#### `set_private`
Mark a conversation as private (hidden from admin view).
```json
{
   "type": "set_private",
   "payload": {
      "conversation_id": 42,
      "is_private": true
   }
}
```
Response: `set_private_response`

With `conversation_id: 0`, it applies to the connection's active conversation when there is
one (a voice turn may have created it meanwhile) and replies as above. With no conversation
yet, it sets the privacy of the next conversation this connection creates, typed or voice, and
gets no reply. That lasts until a conversation is created or loaded or the session is cleared,
and lives on the connection: send it again after a reconnect.

When marking private, the response also carries `also_private`: the ids of the conversation's
continuations, which went private with it. Then, once any memory extraction in flight has
finished, a `conversation_learned` frame reports what the conversation already taught, so the
client can offer to forget it.

#### `set_pinned`
Pin or unpin a conversation.
```json
{
   "type": "set_pinned",
   "payload": {
      "conversation_id": 42,
      "is_pinned": true
   }
}
```
Response `set_pinned_response`: `{success: true, conversation_id, is_pinned, message}`, or
`{success: false, error}` (`"Missing conversation_id or is_pinned"`, `"Conversation not
found"` for one that isn't the user's, `"Failed to update pinned state"`). Other tabs aren't
told; they see it on their next `list_conversations`.

#### `conversation_learned_request`
Ask again what a private conversation taught (the same report `set_private` sends).
```json
{"type": "conversation_learned_request", "payload": {"conversation_id": 42}}
```
Response: `conversation_learned`
```json
{
   "type": "conversation_learned",
   "payload": {
      "conversation_id": 42,
      "success": true,
      "memories": 5,
      "facts": 3,
      "outdated": 1,
      "summaries": 1,
      "preferences": 1,
      "relations": 2,
      "restored": 1
   }
}
```
- `memories` is what the Memory panel lists (facts, summaries and preferences); `outdated` are
  older or duplicate versions of facts, which it doesn't show; `relations` are links in the entity
  graph; `restored` are older values those facts replaced, which a forget makes current again.
- A conversation that isn't the user's, or isn't private, gets `success: false` with `error`.
  `success: false` with `busy: true` means another of the user's memory requests is still
  running: ask again shortly. `success: false` can also come with neither (the count couldn't
  be made or started).

#### `forget_conversation_memories`
Forget what a conversation taught (private or not; the user's own conversations only).
```json
{"type": "forget_conversation_memories", "payload": {"conversation_id": 42}}
```
Response: `forget_conversation_memories_response`, with `conversation_id`, `success`, and on
success the counts removed (the same fields as `conversation_learned`); on failure `error`
says why (memory still being saved from the conversation, too many continuations, busy).

#### `reassign_conversation`
Reassign a conversation to a different user. **Admin only.**
```json
{
   "type": "reassign_conversation",
   "payload": {
      "conversation_id": 42,
      "new_user_id": 3
   }
}
```
Response: `reassign_conversation_response`

#### `export_conversation`
One of the user's conversations as a self-contained JSON document, for a download.
```json
{ "type": "export_conversation", "payload": { "conversation_id": 42, "format": "json" } }
```
- `conversation_id`: required. The user's own conversations only, an admin's too.
- `format`: optional, `"json"` (default) or `"html"`, echoed back. The server sends the same JSON
  document either way; the client renders HTML from it.
- A request without a `payload` gets no reply.

Response:
```json
{
   "type": "export_conversation_response",
   "payload": {
      "success": true,
      "format": "json",
      "data": {
         "exported_at": "2026-11-27T15:00:00Z",
         "dawn_version": "0.1.0",
         "conversation": {
            "id": 42, "title": "Trip planning",
            "created_at": "2026-11-20T10:00:00Z", "updated_at": "2026-11-20T10:30:00Z",
            "message_count": 12, "origin": "webui", "is_private": false,
            "continued_from": null,
            "llm_settings": { "follows_default": true, "llm_type": "cloud", "cloud_provider": "claude",
                              "model": "…", "thinking_mode": "auto", "reasoning_effort": "medium" }
         },
         "messages": [
            { "role": "user", "content": "Find me a train to the coast.", "timestamp": "2026-11-20T10:00:05Z" }
         ]
      }
   }
}
```
- Times are ISO 8601 UTC strings. `llm_settings` has the shape of `load_conversation`'s.
- `messages`: every stored message in order (tool rows included), with `email_ref` on a user row
  that asked about an attached email and `stopped: true` on a reply the user stopped. Request
  context the daemon adds for the model is left out, and so is image data (an image appears as
  its marker in `content`).
- The whole document is one frame.

On failure `success: false` with `error` "Missing conversation_id", "Conversation not found",
"Access denied" (another user's), "Database error", "Failed to load messages", "Conversation has
N messages, exceeding export limit of M" (`[webui] export_max_messages`, default 5000; 0 means no
limit) or "Another export is in progress, try again": the daemon runs one export at a time, for
all users.

#### `search_conversations`
Search through conversation history.
```json
{
   "type": "search_conversations",
   "payload": {
      "query": "weather forecast"
   }
}
```
Response: `search_conversations_response`

#### `save_message`
Save a message to the current conversation in the database. Used by the client to
persist the **assistant's final answer** (role `assistant`).
```json
{
   "type": "save_message",
   "payload": {
      "conversation_id": 42,
      "role": "assistant",
      "content": "The weather is sunny."
   }
}
```
- **`role: "user"` is a no-op** (answers success, writes nothing). The daemon is the
  sole writer of user rows — every typed/text-dispatch user turn is persisted server-side
  in `text_input_dispatch.c`, image markers included, and the turn echoes `server_saved: true`.
  A client should not send user-role saves; a stale one is accepted-and-dropped so it cannot
  double-write. `system`/`tool` roles are rejected outright (daemon-owned).

Response: `save_message_response`

#### `update_context`
Update context token counts for a conversation in the database.
```json
{
   "type": "update_context",
   "payload": {
      "conversation_id": 42,
      "context_tokens": 1500,
      "context_max": 8192
   }
}
```
No response (fire-and-forget).

#### `lock_conversation_llm`
Store a new conversation's LLM settings with its first message (only while it has none).
```json
{
   "type": "lock_conversation_llm",
   "payload": {
      "conversation_id": 42,
      "llm_settings": {
         "llm_type": "cloud",
         "cloud_provider": "claude",
         "model": "claude-sonnet-4-5",
         "thinking_mode": "enabled",
         "reasoning_effort": "medium"
      }
   }
}
```
- Empty `llm_type`, `cloud_provider` and `model`: the conversation has no selection of
  its own and follows the default (`follows_default`); only its reasoning is stored.
- Response: `lock_conversation_llm_response`

#### `continue_conversation`
Continue a conversation after context compaction (create new DB entry linked to old).
```json
{
   "type": "continue_conversation",
   "payload": {
      "conversation_id": 42,
      "summary": "Previous conversation summary..."
   }
}
```
Response: `continue_conversation_response`

#### `clear_session`
Clear the current session's conversation context (without saving).
```json
{"type": "clear_session"}
```
Response: `clear_session_response`

---

### Memory Management (Authenticated)

#### `get_memory_stats`
Get memory system statistics (fact/preference/summary counts).
```json
{"type": "get_memory_stats"}
```
Response: `get_memory_stats_response`

#### `list_memory_facts`
List stored memory facts for the current user.
```json
{
   "type": "list_memory_facts",
   "payload": {
      "limit": 50,
      "offset": 0,
      "sort": "confidence"
   }
}
```
`sort` (optional): `"confidence"` (default — highest confidence first), `"created_desc"`
(newest first), or `"created_asc"` (oldest first). An absent or unrecognized value uses
the default.

Response: `list_memory_facts_response`, with `success` (and `error` on failure), `facts`,
`count` (facts in this page) and `has_more` (the page was full: ask again with a larger
`offset`).  Each fact:

| Field | Meaning |
|---|---|
| `id` | The fact's id |
| `fact_text` | The fact |
| `confidence` | 0 to 1 |
| `source` | How it was stored, raw (`explicit`, `inferred`, `import`, ...) |
| `kind` | How it was learned, as the panel shows it: `told`, `inferred`, `imported`, or `noted` (filed from a note). Absent when none applies |
| `created_at`, `last_accessed` | Unix seconds |
| `access_count` | Times it was recalled |
| `source_conversation_id`, `source_msg_id_start`, `source_msg_id_end` | The conversation and message range it was learned from; present only when known |

#### `list_memory_preferences`
List stored user preferences.
```json
{"type": "list_memory_preferences"}
```
Response: `list_memory_preferences_response`

#### `list_memory_summaries`
List conversation summaries.
```json
{
   "type": "list_memory_summaries",
   "payload": {
      "limit": 50,
      "offset": 0,
      "sort": "created_desc"
   }
}
```
`sort` (optional): `"created_desc"` (default — newest first) or `"created_asc"` (oldest
first). Absent or unrecognized uses the default.

Response: `list_memory_summaries_response`

#### `search_memory`
Search through stored memories.
```json
{
   "type": "search_memory",
   "payload": {
      "query": "favorite color"
   }
}
```
Response: `search_memory_response`

#### `list_memory_entities`
List entities (the Graph tab), most mentioned first.
```json
{"type": "list_memory_entities", "payload": {"limit": 20, "offset": 0}}
```
Response: `list_memory_entities_response`, with `entities`, `count` and `has_more`. Each
entity (the same shape as the `entities` of `search_memory_response`) is a card: the entity and
its aliases. Aliases are not listed on their own, but `count` and `has_more` count the entity rows
read, aliases included: advance `offset` by `count`, not by `entities.length`.

| Field | Meaning |
|---|---|
| `id`, `name`, `canonical_name`, `entity_type` | The entity |
| `mention_count`, `first_seen`, `last_seen` | Across its aliases |
| `alias_count`, `is_user_self` | Linked aliases; whether it is the user |
| `relations` | Its relations, outgoing first, then incoming from outside the card. Each has `relation`, `object_name`, `object_entity_id`, `direction` (`out`/`in`), `confidence`, `mention_count`, and `valid_from`/`valid_to` when set. For `in`, `object_name`/`object_entity_id` name the other end's card |
| `relations_total` | All of the card's relations. At most 50 per direction are sent, most seen first; when `relations_total` is larger, the rest exist but weren't sent |

#### The entity graph: delete, merge, aliases, proposals

These edit the Graph tab's entities. Every verb is scoped to the logged-in user: an id
that isn't the caller's reads as not found. Replies carry `success` and, on failure,
`error` (a human-readable string, not a code). A request without a `payload` gets no reply
for `delete_memory_entity`, `merge_memory_entities`, `entity_link_request`,
`entity_unlink_request` and `entity_proposal_resolve_request`.

An entity can be an **alias** of another (its **canonical**): a soft link that keeps both
rows and can be split again. A merge or a hard link removes the source row for good.

##### `delete_memory_entity`
Delete an entity and every relation that names it (as subject or object).
```json
{ "type": "delete_memory_entity", "payload": { "entity_id": 812 } }
```
Response: `delete_memory_entity_response` with `success` and `message` ("Entity deleted"),
or `error`: "Missing entity_id", "Entity not found", "Failed to delete entity". The id is not
echoed back. The delete counts as the user's removal: what memory had put from it into the
user's conversations is withdrawn, in the background.

##### `merge_memory_entities`
Merge one entity into another for good: the source's relations, the facts filed under it and
its contacts move to the target, duplicates that creates are dropped, the target takes the
source's mention count and seen range, and the source row is deleted. Not undoable (use
`entity_link_request` for a link that can be split).
```json
{ "type": "merge_memory_entities", "payload": { "source_id": 812, "target_id": 640 } }
```
Response: `merge_memory_entities_response` with `success` and `message` ("Entities merged"), or
`error`: "Missing source_id or target_id", "Cannot merge an entity with itself", "Entity not
found or not owned by user", "Failed to merge entities". The ids are not echoed back.

##### `entity_aliases_request`
List an entity's current aliases (split links are left out), newest first, at most 64.
```json
{ "type": "entity_aliases_request", "payload": { "entity_id": 640 } }
```
Response:
```json
{
   "type": "entity_aliases_response",
   "payload": {
      "success": true,
      "entity_id": 640,
      "aliases": [
         {
            "link_id": 31,
            "source_entity_id": 812,
            "source_canonical_name": "alice smith",
            "link_kind": "soft",
            "composite_score": 0.93,
            "linked_at": 1791648000
         }
      ]
   }
}
```
| Field | Meaning |
|---|---|
| `link_id` | The link, for `entity_unlink_request` |
| `source_entity_id` | The alias entity; `0` for a hard link, whose source row is gone |
| `source_canonical_name` | The alias's canonical name as it was when linked |
| `link_kind` | `soft` (can be split) or `hard` (can't) |
| `composite_score` | The match score that made the link; `-1` when none was recorded (a link the user made) |
| `linked_at` | Unix seconds |

Errors: "Missing entity_id", "Invalid entity_id" (not positive), "Failed to list aliases". This
verb answers even with no `payload` ("Missing entity_id").

##### `entity_merge_proposal_list_request`
The caller's pending merge proposals (pairs memory judged likely the same entity but not sure
enough to link), newest first, at most 64. No payload; any payload is ignored.
```json
{ "type": "entity_merge_proposal_list_request" }
```
Response:
```json
{
   "type": "entity_merge_proposal_list_response",
   "payload": {
      "success": true,
      "proposals": [
         {
            "proposal_id": 7,
            "source_entity_id": 812,
            "target_entity_id": 640,
            "source_canonical_name": "al smith",
            "target_canonical_name": "alice smith",
            "composite_score": 0.78,
            "proposed_at": 1791648000
         }
      ]
   }
}
```
A name is `""` when its entity has since been deleted. Error: "Failed to list proposals". The
count of pending proposals is pushed as `memory_proposals_changed`.

##### `entity_link_request`
Link one entity as a soft alias of another (the user's own link).
```json
{
   "type": "entity_link_request",
   "payload": { "source_entity_id": 812, "target_entity_id": 640, "reason": "same person" }
}
```
- `source_entity_id`, `target_entity_id` (required): the source becomes an alias of the target.
- `reason` (optional, default `"operator"`): kept in the link's record; cut to 128 bytes, and
  refused when the prompt-injection filter matches it.

Response: `entity_link_response` with `success`, `link_id`, `source_entity_id` and
`target_entity_id`, or `error`: "Missing source_entity_id or target_entity_id", "Reason failed
prompt-injection filter", or "Link failed (entity not found, self-link, or source has
dependents)" (the source is itself the canonical of other aliases).

##### `entity_unlink_request`
Split a soft link: the alias becomes an entity of its own again.
```json
{ "type": "entity_unlink_request", "payload": { "link_id": 31, "reason": "different people" } }
```
- `link_id` (required).
- `reason` (optional, default `"split-by-operator"`): 128 bytes, filtered as for
  `entity_link_request`.

Response: `entity_unlink_response` with `success` and `link_id`, or `error`: "Missing link_id",
"Reason failed prompt-injection filter", "Link not found or already unlinked", and, for any
other failure, "Hard merges cannot be split — use 'dawn-admin memory reextract'".

##### `entity_proposal_resolve_request`
Approve or reject a pending merge proposal. Approving makes the source a soft alias of the target.
```json
{
   "type": "entity_proposal_resolve_request",
   "payload": { "proposal_id": 7, "resolution": "approved" }
}
```
- `proposal_id` (required, positive); `resolution` (required): `approved` or `rejected`.

Response: `entity_proposal_resolve_response` with `success`, `proposal_id`, `resolution`, and
`link_id` when an approval made a link; or `error`: "Missing proposal_id or resolution",
"Invalid proposal_id or resolution", "resolution must be 'approved' or 'rejected'", "Proposal not
found or already resolved", or "Approve failed (entity not found, self-link, or source has
dependents); proposal still pending". On success every connection of the user (satellites
included) gets
`memory_proposals_changed` with the new pending count.

#### `get_memory_fact_source`
Return the verbatim source conversation messages a fact was extracted from (memory provenance).
```json
{
   "type": "get_memory_fact_source",
   "payload": {
      "fact_id": 5
   }
}
```
Response: `get_memory_fact_source_response`

On success the payload carries the source range and up to 500 user/assistant
messages (system/tool messages are omitted):
```json
{
   "type": "get_memory_fact_source_response",
   "payload": {
      "fact_id": 5,
      "success": true,
      "conversation_id": 1222,
      "msg_id_start": 1,
      "msg_id_end": 24682,
      "messages": [
         { "id": 24663, "role": "user", "content": "…", "created_at": 1757370000 }
      ]
   }
}
```
A user message that asked about an attached email carries `email_ref`, and a stopped reply
`stopped: true`, as in `load_conversation_response`. An empty `messages` array with `success: true` means the range
held no user/assistant messages (only system/tool). On failure, `success` is `false`
and `reason` is one of: `not_available` (no provenance recorded, fact not
found, source conversation deleted, or private), `forbidden` (the source
conversation is not owned by the caller), `invalid_range` (the stored
provenance range is invalid), or `error` (server/DB failure).

#### `delete_memory_fact`
Delete a specific memory fact.
```json
{
   "type": "delete_memory_fact",
   "payload": {
      "fact_id": 5
   }
}
```
Response: `delete_memory_fact_response` with `success`, the `fact_id` asked for, and `error` on failure ("Memory is still being
saved; try again in a moment" while an extraction for the user runs past the wait; "Memory is busy with
another change; try again shortly" when the user already has several deletes running). The duplicates
merged into it are deleted with it; the values it replaced are current again: `restored_count` says how
many, `restored` lists the text of the first few. It runs on a worker.

#### `list_memory_superseded`
The caller's merged or replaced facts (recall no longer shows them), newest first, at most 50.
`query` (optional) keeps those whose text contains it.
```json
{
   "type": "list_memory_superseded",
   "payload": { "query": "email" }
}
```
Response: `list_memory_superseded_response`
```json
{
   "type": "list_memory_superseded_response",
   "payload": {
      "success": true,
      "facts": [
         {
            "id": 42,
            "text": "...",
            "keeper_id": 17,
            "keeper_text": "...",
            "replaced": false,
            "superseded_at": 1791648000
         }
      ],
      "truncated": false
   }
}
```
`truncated` is true when there may be more than the 50 listed (search to find them).
`superseded_at` is 0 when unknown. `replaced` is true for an older value a newer fact replaced, false
for a duplicate merged into it; the kept fact (`keeper_text`) may itself have been superseded since. `query` is echoed back,
so a client can drop a reply to an earlier search.

#### `restore_memory_fact`
Bring a merged or replaced fact back, with the relations its replacement closed; a merged fact also
takes back what the merge moved onto the kept fact (what it stated, the values it replaced).
```json
{
   "type": "restore_memory_fact",
   "payload": { "fact_id": 42 }
}
```
Response: `restore_memory_fact_response` with `success`, `fact_id`, `both_current` (the newer value
that replaced it stays current too) and `error` on failure
(the fact isn't the caller's, or was already restored or deleted).

#### Consolidation: what memory learned per batch

With `memory_batches` advertised.  Each time memory consolidates a conversation (a *batch*, run
when the conversation ends or goes quiet with `[memory] consolidator = "agent"`), DAWN records
what it did: each fact it added, reinforced, merged or replaced, each relation it closed, each
entity it linked, each preference it set, and each candidate it dropped or set aside for later.
A client shows a batch as a marker in its conversation and in a Learned list, and lets the user
undo it, keep a candidate it dropped, or forget a fact it suggested forgetting.  Batches are not
messages: they never appear in `messages`, the history, or what the model is sent.

##### The batch object

```json
{
   "batch_id": 412,
   "conversation_id": 1703,
   "conversation_title": "Trip planning",
   "from_message_id": 26230,
   "after_message_id": 26244,
   "created_at": 1793000000,
   "path": "agent",
   "status": "applied",
   "outside": true,
   "outside_from": "email",
   "counts": {"added": 3, "reinforced": 2, "merged": 1, "replaced": 1, "closed": 1,
              "linked": 0, "proposed": 0, "pref_set": 1, "dropped": 4, "deferred": 1,
              "suggested_forget": 0, "refused": 0},
   "ops": [ ]
}
```

- `conversation_id`: the conversation whose messages hold `after_message_id`: the one the batch
  read, never the root of a continuation chain (a continued conversation is its own and gets its
  own batches); `null` for a batch with none.
- `conversation_title`: that conversation's title as it is when the frame is built (at most 255
  bytes; may be `""`); `null` when `conversation_id` is `null` or the conversation is gone.  A
  rename sends no `memory_batch`: update shown batches from `conversation_renamed`.  Absent on
  daemons before 2026-11-17.
- `from_message_id`: the first message the batch learned from.
- `after_message_id`: place the marker after this.  It may name a row the client doesn't render
  as a bubble (a tool row, an intermediate assistant row of a tool loop, a request-context row);
  message ids grow within a conversation, so **place the marker after the greatest rendered
  message id ≤ `after_message_id`**.  When none is rendered (a range the client hasn't loaded),
  show the batch only in the Learned list.  `0` or absent: the Learned list only.  Rows are never
  renumbered (compaction changes what the model is sent, not what is stored); a retracted turn's
  rows are deleted, which the ≤ rule absorbs.
- `path`: `agent`, or `agent_partial` when the run didn't finish and only what can't lose a
  stored item was applied, or the batch filled its record (256 ops) and what didn't fit wasn't
  applied.
- `status`: `applied` · `partial` (some ops undone, some not) · `undone` (every op that can be
  undone was).
- `outside`: the batch read outside text (an email, a web page, ...).  `outside_from`: what it
  read, `null` when `outside` is false: `web` · `email` · `research` (a research report) ·
  `messaging` (a message from a chat channel) · `document` · `calendar` (a calendar event) ·
  `mcp` (an MCP server's result) · `other` (any other tool's result, DAWN's own posts, a device's
  data).  Show it once per batch ("Read outside text: an email").  A memory DAWN recalled into the conversation doesn't count,
  whatever it was learned from, and neither does the user's own document (a note, or one marked
  My own text: see Document Library).
- `counts`: the batch's ops by kind (a link that became a proposal counts as `proposed`); refused
  ops count in `refused` only and are not listed.

##### Op rows

```json
{
   "op_id": 9001,
   "op": "merged",
   "status": "applied",
   "outside": false,
   "outside_from": null,
   "fact_id": 7690,
   "relation_id": null,
   "entity_id": null,
   "text": "...",
   "target_fact_id": 7702,
   "target_entity_id": null,
   "target_text": "...",
   "target_status": "current",
   "reason": "...",
   "reason_by": "model",
   "action": "undo"
}
```
The row of the fact kept (7702 here), when this batch added or reinforced it, also carries what was
merged into it:
```json
{"op_id": 9000, "op": "added", "fact_id": 7702, "sources": [{"fact_id": 7690, "text": "..."}]}
```

- `op`: `added` · `reinforced` · `merged` · `replaced` · `closed` · `linked` · `pref_set` ·
  `dropped` · `deferred` · `suggested_forget`.
- `status`: `applied` · `proposed` (a link that became a merge proposal for the user to decide) ·
  `undone` · `stale` (an undo found the row changed since; nothing was done) · `kept` (a dropped or
  set-aside candidate the user kept) · `consumed` (a set-aside candidate a later batch took up) ·
  `expired` (a set-aside candidate past its 30 days).  There is no `forgotten` status: forgetting
  a fact removes every row naming it, so an acted-on suggestion leaves the list.
- `outside`, `outside_from`: the batch's taint (a kept or set-aside candidate: its own batch's).
  Tag a row only when they differ from the batch's.  A stored fact's own taint is read from the
  fact.
- `fact_id`: the op's own fact; absent or `null` for `closed`, `linked` and `pref_set`.
- `text`: the op's item when it was recorded: fact text, a relation as "subject predicate
  object", an entity name, a preference as "category: value"; `null` for a candidate dropped
  because the user forgot it.
- `target_fact_id`: `merged`: the fact kept; `replaced`: the newer value; `closed`: the fact
  whose value closed it (if any).  `target_entity_id`: `linked`: the entity linked to.
- `target_text`: the counterpart when recorded (it may be gone since).  `target_status`:
  `current` · `superseded` · `gone`, the counterpart now (computed when the list is built);
  absent when the op has none.
- `sources`: on this batch's `added` or `reinforced` row of a fact a merge of this batch landed on:
  the facts merged into it.  Absent otherwise (a merge into a stored fact the batch didn't touch
  has no row to carry it; its `merged` row names the fact kept).
- `reason`: one line.  **Render it as text (`textContent`), never as markup.**  `reason_by` says
  who wrote it: `model` (the model's words: show them as such, e.g. "Model's reason:") or `dawn`
  (DAWN's own, e.g. "not reached"); absent with no reason, or on a row recorded before it was kept.
- `action`: `undo` (an applied op that can be undone) · `keep` (an applied dropped candidate, or
  a set-aside one open or expired) · `forget` (an applied `suggested_forget`: send the existing
  `delete_memory_fact` with its `fact_id`, which records the forget) · `null`.
- `text`, `target_text` and `reason` are cut when recorded, on a character boundary: 511, 511 and
  200 bytes.

##### `list_memory_batches`
```json
{"type": "list_memory_batches", "payload": {"conversation_id": 1703, "limit": 20}}
{"type": "list_memory_batches", "payload": {"before_batch_id": 380, "limit": 20}}
```
Keyset pages, newest first: `before_batch_id` (absent: from the newest), `limit` (default 20, at
most 50), and `conversation_id` (absent: every conversation's).  Response:
```json
{
   "type": "list_memory_batches_response",
   "payload": {
      "conversation_id": 1703,
      "before_batch_id": null,
      "limit": 20,
      "batches": [ ],
      "has_more": true,
      "next_before_batch_id": 371,
      "error": null
   }
}
```
The request's form is echoed (`conversation_id` only when asked for).  `next_before_batch_id` is
present only when `has_more` is true; send it as the next request's `before_batch_id`.  A page
also stops early when its batches reach 256 KB (then `has_more` is true).  A conversation that
doesn't exist and one that isn't the caller's answer the same: `"error": "Not found"`, no batches.

##### `undo_memory_batch`
```json
{"type": "undo_memory_batch", "payload": {"batch_id": 412, "op_id": 9001}}
```
`op_id` absent: every op of the batch that can be undone, the last applied first (a closed relation
last, after the value that closed it).  An `op_id` that is present but not a number, `null`
included, is "Not found", never the whole batch (so is a request with no `op_id` that had
any `null` member).  Each op is checked against what it left; if anything else changed
that row since (a later batch's op still in effect on the same fact, relation, link or
preference, or, for a merge, on the fact kept; a merge onto a fact it added; another conversation
teaching it; a restore), it is left as it is and marked `stale`.  A closed relation stays closed while another value of it is open.
What an undo removes is withdrawn from the conversations it was sent into, like a forget, but no
forgotten record is made: memory may learn it again.  It waits for the user's running memory
extraction, on a worker; the reply comes when it ends:
```json
{
   "type": "undo_memory_batch_response",
   "payload": {"batch_id": 412, "op_id": 9001, "status": "ok", "undone": [9001], "stale": [],
               "error": null}
}
```
`status`: `ok` (everything asked for was undone; undoing again is `ok` too) · `partial` (a whole
batch with some ops stale: `undone` and `stale` list them) · `stale` (nothing could be undone:
"This memory changed since; nothing was undone.") · `error` ("Not found"; "This one can't be
undone."; "Couldn't undo this right now. Try again in a moment." when the extraction wait or the
database refused, or an undo or keep of the user's is already running).  A batch or op that
doesn't exist, or isn't the caller's, or an op named with another batch's id, is "Not found".

##### `keep_memory_op`
```json
{"type": "keep_memory_op", "payload": {"batch_id": 412, "op_id": 9010}}
```
Stores a dropped or set-aside candidate through the same path and checks a batch stores with,
carrying the outside text it was recorded with (a kept candidate from an email is still marked as
from outside text, never as the user's own word).  The op becomes `kept` and a new `added` op
(or `reinforced`, when it says a stored fact again) joins the batch, which can be undone.  On a
worker, like an undo:
```json
{
   "type": "keep_memory_op_response",
   "payload": {"batch_id": 412, "op_id": 9010, "status": "ok", "fact_id": 8140, "error": null}
}
```
A candidate whose text a current fact already says is only marked `kept`, its `fact_id` that fact
(nothing new joins the batch).
`status`: `ok` · `error` ("Not found"; "You asked me to forget this; it stays forgotten." when a
forgotten record says it; "This batch is full; nothing more can be kept from it." when the batch
holds 256 ops; "This one can't be kept." when it was kept already or is about the assistant; "Couldn't
keep this right now. Try again in a moment." when the extraction wait, the store or its record failed,
or an undo or keep of the user's is already running: nothing is stored then).

##### `memory_batch` (push)
```json
{"type": "memory_batch", "payload": { }}
```
The batch object, to every browser tab of the user, after a batch is recorded, after every undo or
keep (before the reply), and after a fact one of its ops names is forgotten (a Forget from a
suggestion removes that row).  Replace a batch already shown by `batch_id`.

A batch a forget removed (its conversation forgotten) comes as its id alone:
```json
{"type": "memory_batch", "payload": {"batch_id": 412, "removed": true}}
```
Drop it (its row and its conversation marker) wherever it is shown.

#### `export_memories`
The user's memories, for a backup or for another assistant.
```json
{ "type": "export_memories", "payload": { "format": "json" } }
```
- `format`: optional, `"json"` (default) or `"text"`; anything else is refused with "Invalid
  format (use 'json' or 'text')". The payload may be left out.

`json` response, lossless enough to import back:
```json
{
   "type": "export_memories_response",
   "payload": {
      "success": true, "format": "json", "fact_count": 1, "pref_count": 1, "entity_count": 1,
      "data": {
         "version": 1, "format": "dawn_memory", "exported_at": 1785000000,
         "facts": [ { "fact_text": "Alice prefers window seats", "confidence": 0.9,
                      "source": "explicit", "created_at": 1784000000, "origin": 0, "outside": 0 } ],
         "preferences": [ { "category": "communication", "value": "brief answers", "confidence": 0.8,
                            "source": "inferred", "created_at": 1784000000, "outside": 0 } ],
         "entities": [ { "name": "Bob", "entity_type": "person", "mention_count": 4, "first_seen": 1783000000,
                         "relations": [ { "relation": "works_at", "object_name": "Example Corp",
                                          "confidence": 0.8, "mention_count": 2 } ] } ]
      }
   }
}
```
- `outside`: where the item's text came from (`outside_kind_t`, as on documents). `origin`: how a
  fact was learned.
- An entity's `relations` include its aliases' (an alias has no entry of its own).

`text` response: `{success, format: "text", data, fact_count, pref_count}`, where `data` is one
string: a `# My Memories` heading, a `## Facts` list (`- <fact>`) and a `## Preferences` list
(`- <category>: <value>`). No entities.

The whole export is one frame, however large. On failure `success: false` with "Failed to read
memories", "Memory allocation failed" or "Memories changed during the export; try again".

#### `import_memories`
Add memories from a DAWN JSON export or from pasted text (one fact per line, as another
assistant lists them). Preview first, then commit.
```json
{ "type": "import_memories", "payload": { "format": "text", "text": "- Alice prefers window seats\n- Alice is allergic to peanuts", "commit": false } }
```
```json
{ "type": "import_memories", "payload": { "format": "json", "data": { "facts": [ … ], "preferences": [ … ] }, "commit": true } }
```
- `format`: optional, `"text"` (default) or `"json"`.
- `commit`: optional, default false. False is a preview: nothing is written and the reply lists
  what would be imported.
- `text` (format text): required, at most 256 KB ("Text too large (256KB max)"). Read line by
  line, at most 200 lines; a line starting `#` is skipped and a leading `- ` or `* ` is dropped;
  a line under 3 characters is skipped as empty. Each fact gets confidence 0.7.
- `data` (format json): required; an export's `data` object. Up to 200 of its `facts` and 200 of
  its `preferences` are read; `entities` are ignored. A fact's `confidence` (clamped to 0–1,
  default 0.8) and `outside` are kept; a missing or out-of-range `outside` is stored as not known.
- A fact is cut to 511 bytes and a preference to a 31-byte category and 255-byte value.
- A request without a `payload` gets no reply.

Response:
```json
{
   "type": "import_memories_response",
   "payload": {
      "success": true, "committed": false,
      "imported_facts": 2, "imported_prefs": 0,
      "skipped_dupes": 0, "skipped_empty": 0, "skipped_blocked": 0, "skipped_failed": 0,
      "preview": [
         { "type": "fact", "text": "Alice prefers window seats", "confidence": 0.7 },
         { "type": "preference", "category": "communication", "value": "brief answers" }
      ]
   }
}
```
- `preview`: only when `commit` is false.
- `skipped_dupes`: a fact the user already has (committing marks the existing one as stated by
  the user). `skipped_blocked`: refused by the memory filter. `skipped_failed`: a fact whose save
  failed on commit. A preference is upserted by category (no duplicate check).
- Imported facts are stored without an embedding; they become searchable by meaning once the
  background embedder reaches them.
- On failure `success: false` with "Invalid format (use 'json' or 'text')",
  "Missing data", "Missing text", "Empty text", "Text too large (256KB max)" or "Memory
  allocation failed".

#### `delete_memory_preference`
Delete one of the user's preferences, by its category.
```json
{
   "type": "delete_memory_preference",
   "payload": {
      "category": "communication"
   }
}
```
Response: `delete_memory_preference_response` with `success` and `message` ("Preference deleted"),
or `error`: "Missing category", "Preference not found", "Failed to delete preference". A request
without a `payload` gets no reply.

#### `delete_memory_summary`
Delete a specific conversation summary.
```json
{
   "type": "delete_memory_summary",
   "payload": {
      "summary_id": 7
   }
}
```
Response: `delete_memory_summary_response` with `success` and `message` ("Summary deleted"), or
`error`: "Missing summary_id", "Summary not found", "Failed to delete summary". A request without
a `payload` gets no reply.

#### `delete_all_memories`
Delete all memories for the current user. Requires confirmation.
```json
{
   "type": "delete_all_memories",
   "payload": {
      "confirm": "DELETE"
   }
}
```
Response: `delete_all_memories_response` with `success` and `error` on failure. It runs on a worker and
waits for a memory extraction in progress; if one doesn't finish in time it fails with "Memory is still
being saved; try again in a minute" and nothing is deleted. On success the user's conversations count as
extracted, so their memories aren't learned again from them.

---

### Contacts (Authenticated)

Each user's contacts: an email address, phone number or postal address attached to a person in
their memory (an entity). Every reply is `<type>_response` with `success` and, on failure,
`error`. A request in this section without a `payload` (except `contacts_list`) gets no
reply. `entity_set_photo` and `entity_ensure`, at the end, set the person's photo the panel
shows.

**A contact:**
```json
{"contact_id": 7, "entity_id": 88, "entity_name": "Bob", "field_type": "email",
 "value": "bob@example.com", "label": "work", "photo_id": "img_0123"}
```
`photo_id` only when the person has a photo.

**Values** (`contacts_add`, `contacts_update`): `field_type` is `email`, `phone` or `address`.
`value` at most 255 bytes, `label` at most 31. An email needs `user@domain.tld` and is
lowercased. A phone number may contain digits, spaces, `-`, `(`, `)` and `.`, and a leading
`+`; it is stored as E.164: `+` and 7 to 15 digits, or a 10-digit US number (or 11 digits
starting with `1`) given `+1`. An address isn't checked.

#### `contacts_list`
```json
{"type": "contacts_list", "payload": {"field_type": "phone", "limit": 20, "offset": 0}}
```
All optional: `field_type` filters; `limit` 1–100 (default 20); `offset` default 0. Ordered by
name. Response: `{success, contacts: [...], has_more}`.

#### `contacts_search`
Contacts by person name.
```json
{"type": "contacts_search", "payload": {"name": "bob", "field_type": "email"}}
```
`name` required (`"Missing 'name' field"`). Matches the whole name first, then a whole word of
it or of another name of the same person, then a part of a word; at most 20. An empty name, or
one over 200 bytes, finds nothing. `field_type` optional. Response: `{success, contacts}`.

#### `contacts_add`
```json
{
   "type": "contacts_add",
   "payload": {
      "entity_name": "Bob",
      "field_type": "email",
      "value": "bob@example.com",
      "label": "work"
   }
}
```
- `field_type`, `value` required; `label` optional.
- The person: `entity_id` (one of the user's entities), or `entity_name` (required when there's
  no `entity_id`). A name that is exactly one of the user's entities uses it. Otherwise, when
  people with similar names exist, nothing is added and the reply asks which one:
  ```json
  {"success": false, "needs_disambiguation": true, "entered_name": "Bob",
   "candidates": [ {"id": 88, "name": "Bob Smith", "mention_count": 12} ]}
  ```
  Send again with the chosen `entity_id`, or with `force_create: true` to make a new person of
  that name. With no similar person, a new one is made.
- An `entity_id` that isn't the user's → `"Failed to add contact"`; an `entity_id` of 0 or less →
  `"Failed to find or create entity"`.

Response: `{success: true, entity_id, entity_created}`.

#### `contacts_update`
Replace a contact's type, value and label.
```json
{
   "type": "contacts_update",
   "payload": {"contact_id": 7, "field_type": "phone", "value": "555 010 0199", "label": "mobile"}
}
```
`contact_id`, `field_type`, `value` required; `label` optional (absent clears it). Checked as
`contacts_add`. Not the user's or unknown → `"Failed to update contact"`. Response `{success}`.

#### `contacts_delete`
```json
{"type": "contacts_delete", "payload": {"contact_id": 7}}
```
Not the user's or unknown → `"Failed to delete contact"`. Response `{success}`.

#### `contacts_search_entities`
People in the user's memory whose names match, to pick one for `contacts_add`.
```json
{"type": "contacts_search_entities", "payload": {"query": "bo"}}
```
`query` required; empty gives no results. At most 10, persons only. Response:
`{success, entities: [{id, name, entity_type: "person", mention_count}]}`.

#### `entity_set_photo`
Set or clear the photo shown for an entity (the contacts panel's avatar).
```json
{ "type": "entity_set_photo", "payload": { "entity_id": 640, "photo_id": "img_0123abcd" } }
```
- `entity_id` (required): one of the caller's entities.
- `photo_id` (optional): an uploaded image's id, which must be the caller's. Absent, `null` or
  `""` clears the photo.

The new photo is kept for good (exempt from image retention); a photo it replaces goes back to
normal retention.

Response: `entity_set_photo_response` with `success`, `entity_id` and `photo_id` (when one was
set), or `error`: "Missing 'entity_id' field", "Image not found", "Image does not belong to you",
"Entity not found or not owned by you", "Database error".

#### `entity_ensure`
Find the caller's entity by name, or create it as a person, so a photo can be set on someone
memory hasn't met yet.
```json
{ "type": "entity_ensure", "payload": { "name": "Alice Smith" } }
```
- `name` (required, non-empty). It is matched by its canonical form (ASCII lower-cased, trailing
  spaces trimmed, at most 63 bytes). A new entity has type `person`; an existing one keeps its type, counts one more
  mention, and takes the new `name` when it is longer than its own.

Response: `entity_ensure_response` with `success`, `entity_id` and `created` (a new entity), or
`error`: "Missing 'name' field", "Failed to create entity".

---

### Document Library (Authenticated)

The document library panel: uploaded documents and notes the model can search. Every request
needs a login (else `UNAUTHORIZED`); each user works on their own documents and reads the global
ones. Every reply is `<request>_response` with `{success, error?, …}` (`set_document_own`, below,
is the exception). `doc_library_delete`, `_index`, `_toggle_global`, `_note_save`,
`_note_update`, `_doc_update`, `_version_list` and `_version_restore` need a `payload`: a
request without one gets no reply. A note is a one-chunk document whose filename is its label
and whose `filetype` is `"note"`.

In `doc_library_list_response`, each row of a document the requester owns carries:
- `outside`: where its text came from (`outside_kind_t`, `include/core/outside_kind.h`): `0` the
  user's own words, `5` a document (every upload, attachment or saved text), `3` a research
  report; a note written in a turn that read outside text carries that text's kind.
- `own`: whether a chunk of it recalled into a turn counts as the user's own words: the user
  vouched for it (`set_document_own`), or `outside` is 0 or 8.  A note starts own, anything else
  not.  The panel's switch shows this value.  Rows of another user's document (a global one, or
  the admin All Users view) carry neither, and show no switch.

#### `set_document_own`
Vouch for one of the user's documents as their own text, or take it back.  On sets the vouch and
keeps `outside` as it is (a research report stays 3, an attachment 5).  Off clears the vouch, and
an `outside` of 0 or 8 becomes 5 (the user says it isn't their words); any other kind stays.  So
the response's `own` is the value asked for.  Only the document's owner can, from the panel: no
tool action sets it.
```json
{"type": "set_document_own", "payload": {"document_id": 42, "own": true}}
```
Response:
```json
{"type": "set_document_own_response", "payload": {"document_id": 42, "own": true, "outside": 5}}
```
or `{"document_id": 42, "error": "..."}` (a missing field, or not the user's document).  A save,
edit or append by the model in a turn that read outside text clears the vouch; the next list
shows it.  A deleted document recovered comes back without it.

#### `doc_library_list`
A page of the user's documents and notes, newest first, or the results of a search.
```json
{ "type": "doc_library_list", "payload": { "scope": "notes", "limit": 20, "offset": 0 } }
```
All members optional (the payload may be left out):
- `limit`: 1–20, default 20; any other value is treated as 20.
- `offset`: rows to skip, default 0.
- `scope`: `"documents"` (no notes), `"notes"` (notes only); anything else, or absent, both.
- `show_all`: admins only, ignored for others: every user's documents, each row with `user_id`
  and `owner_name`. `scope` is ignored with `show_all`.
- `query`: a word search over labels and text (BM25). One page of at most `limit` rows, one per
  document; `offset` and `show_all` are ignored and `has_more` is false.

Without `query` (or `show_all`): the user's own documents and the global ones.

Response:
```json
{
   "type": "doc_library_list_response",
   "payload": {
      "success": true,
      "count": 2,
      "has_more": false,
      "documents": [
         { "id": 42, "filename": "Wifi password", "filetype": "note", "is_note": true,
           "text": "The guest network is example-guest.", "num_chunks": 1,
           "is_global": false, "created_at": 1785000000, "outside": 0, "own": true },
         { "id": 41, "filename": "manual.pdf", "filetype": "pdf", "is_note": false,
           "num_chunks": 18, "is_global": false, "created_at": 1784990000, "outside": 5,
           "own": false, "has_original": true, "original_blob_id": "0123456789abcdef0123456789abcdef" }
      ]
   }
}
```
- `text`: a note's body, so the panel can show and edit it without another request. Documents
  carry none; read one with `doc_library_get`.
- `has_original`, `original_blob_id`: on documents (not notes) the requester owns or that are
  global. The uploaded file is downloadable at `GET /api/documents/original/<original_blob_id>`.
- `outside`, `own`: see above; only on the requester's own documents.
- `has_more`: the page was full (so it can be true when nothing follows).
- On failure `success: false`, `error` "Failed to list documents".

#### `doc_library_get`
A document's full text.
```json
{ "type": "doc_library_get", "payload": { "id": 41 } }
```
Response `{success: true, id, filename, filetype, text}`. One error for every refusal
(missing, another user's private document, or an upload too old to have its full text stored):
`{success: false, id, error: "Document unavailable"}`, so the reply doesn't tell whether an id
exists. `id` is echoed on every reply but this one: a request with no `id` (or no payload)
gets `{success: false, error: "Missing document id"}`.

#### `doc_library_delete`
```json
{ "type": "doc_library_delete", "payload": { "id": 41 } }
```
The owner, or an admin for anyone's. Response `{success: true, id}`, or `success: false` with
"Missing document id", "Document not found", "Permission denied" or "Delete failed". Its passages
are withdrawn from the conversations they were recalled into, and its versions are kept, so it
appears in `doc_library_deleted_list` and can be restored.

#### `doc_library_index`
Add a document from text the client extracted. Uploading the file first (`POST /api/documents`)
returns its stored original's blob id, which the client passes back here.
```json
{
   "type": "doc_library_index",
   "payload": {
      "filename": "manual.pdf", "filetype": "pdf", "text": "…the extracted text…",
      "is_global": false, "original_blob_id": "0123456789abcdef0123456789abcdef"
   }
}
```
- `filename`, `filetype`, `text`: required (stored up to 255 and 15 bytes).
- `is_global`: optional; honoured for admins only, others always add a private document.
- `original_blob_id`: optional, links the document to its uploaded original.

The text is chunked and embedded before the reply. Response `{success: true, id, filename,
num_chunks, failed_chunks?}` (`failed_chunks`: chunks that couldn't be embedded, when any), or
`success: false` with "Missing required fields" or the indexer's message: "Document text is
empty", "Document text exceeds maximum size" (over `[documents] max_index_size_kb`, default
2048 KB), "Document limit reached (N max)" (`[documents] max_indexed_documents` per user,
default 50), "Embedding engine not available", "Document already indexed (id=N)",
"Failed to chunk document text", "No chunks could be embedded", "Failed to create document record", "Memory allocation failed".

#### `doc_library_toggle_global`
Share a document with every user, or stop sharing it.
```json
{ "type": "doc_library_toggle_global", "payload": { "id": 41, "is_global": true } }
```
The owner, or an admin. Response `{success: true, id, is_global}`, or `success: false` with
"Missing id or is_global", "Document not found", "Permission denied" or "Update failed".

#### `doc_library_note_save`
Create a note.
```json
{ "type": "doc_library_note_save", "payload": { "label": "Wifi password", "text": "The guest network is example-guest." } }
```
Both required. Response `{success: true, id, label}`, or `success: false` with "A label and text
are required", "A note with that label already exists" (the user's own notes; edit it with
`doc_library_note_update`), "Note label is empty", "Note text is empty", "Note is too long to
file as a single note — …" (over about 1,000 tokens: index it as a document instead), "Document
limit reached", "Embedding engine not available", "Failed to create note record", "Failed to embed
note text", "Failed to store note chunk" or "Memory allocation failed". A new note is always private.

#### `doc_library_note_update`
Replace a note's label and text; its id stays.
```json
{ "type": "doc_library_note_update", "payload": { "id": 42, "label": "Wifi password", "text": "Updated text." } }
```
All three required. The previous content is kept as a version. Response `{success: true, id,
label}`, or `success: false` with "id, label, and text are required", "Another note already
uses that label", or the indexer's message (as for `doc_library_note_save`, plus a refusal for a
note that isn't the user's). The note keeps its `outside` kind and the user's vouch.

#### `doc_library_doc_update`
Replace a document's full text (re-chunked and re-embedded); its id stays. Notes are refused.
```json
{ "type": "doc_library_doc_update", "payload": { "id": 41, "text": "…the full new text…" } }
```
The owner only. The previous content is kept as a version. Response `{success: true, id}`, or
`success: false` with "a valid id and text are required", "Document not found (or not yours)",
"That item is a note — edit it via doc_library_note_update", or the indexer's message (an
upload too old to have its full text stored asks to be re-saved first).

#### `doc_library_version_list`
A note's or document's earlier versions, newest first (at most 32).
```json
{ "type": "doc_library_version_list", "payload": { "id": 42 } }
```
Response:
```json
{
   "type": "doc_library_version_list_response",
   "payload": {
      "success": true, "id": 42,
      "versions": [ { "id": 901, "filename": "Wifi password", "preview": "The guest network is…", "archived_at": 1785000100 } ]
   }
}
```
`preview`: the start of that version's text (under 160 bytes). Each row's `id` is a version id,
for `doc_library_version_restore`. A document that isn't the user's lists no versions
(`success: true`, empty). `success: false` only for "a valid id is required".

#### `doc_library_deleted_list`
The user's deleted notes and documents that can still be restored, newest first (at most 32).
Payload optional.
```json
{ "type": "doc_library_deleted_list" }
```
Response `{success: true, deleted: [{version_id, filename, preview, archived_at}]}`.

#### `doc_library_version_restore`
Bring back a version.
```json
{ "type": "doc_library_version_restore", "payload": { "version_id": 901 } }
```
- When the item still exists, its content is replaced in place, and its current content is kept
  as a version first (so the restore can be undone).
- When it was deleted, it is created again under the version's label: as a note, or as a
  document when the text is too long for one note. It gets a new `id`, and leaves
  `doc_library_deleted_list`.
- The item takes back the `outside` kind the version had.

Response `{success: true, id, label, recreated}` (`recreated`: it was deleted and is new), or
`success: false` with "a valid version_id is required", "Version not found (or not yours)" or
the indexer's message.

---

### Code Projects

Git repositories indexed into a code graph the model can search (the coding harness). Built only
with code projects (`DAWN_ENABLE_CODE_PROJECTS`); a daemon without it answers these types with
`UNKNOWN_TYPE`. Every request needs a login (else `UNAUTHORIZED`). A user sees their own projects
and the global ones; an admin sees every project. Only a project's importer, or an admin, may
refresh, rebuild, change the branch of or delete it. The payload is optional on every request,
and every request gets a reply.

Replies use `{ok, message}`, not `success` / `error`: `message` is a sentence for people, on
success too. `code_projects_list`, `code_projects_import` and `code_projects_link` reply with
`<request>_response`; `code_projects_refresh`, `_rebuild`, `_set_branch` and `_delete` all reply
with **`code_projects_action_response`**, so a client with two of those outstanding can't tell
the replies apart.

Work runs on a background thread: an accepted import, link, refresh or rebuild is only queued,
and `code_project_status_changed` (below) reports each change of a project's state.

#### `code_projects_list`
```json
{ "type": "code_projects_list" }
```
Response (at most 64 projects):
```json
{
   "type": "code_projects_list_response",
   "payload": {
      "ok": true,
      "projects": [
         { "id": 5, "name": "example-repo", "status": "ready", "status_msg": "",
           "is_global": false, "source_url": "https://git.example.com/alice/example-repo.git",
           "indexed_at": 1785000000, "kind": "clone", "branch": "main", "owned": true }
      ]
   }
}
```
- `status`: `"cloning"`, `"indexing"`, `"ready"` or `"error"` (`status_msg` says why).
- `kind`: `"clone"` (DAWN's own clone of `source_url`) or `"local"` (a linked checkout on the
  server, `code_projects_link`).
- `owned`: whether this user may act on it (always true for an admin).
- `indexed_at`: epoch seconds of the last index, `0` before one.
- On a database failure `ok: false` with an empty `projects`.

#### `code_projects_import`
Clone a remote repository and index it.
```json
{
   "type": "code_projects_import",
   "payload": { "url": "https://git.example.com/alice/example-repo.git", "name": "example-repo", "branch": "main", "global": false }
}
```
- `url`: required.
- `name`: optional; defaults to the URL's last path segment without `.git` (up to 63 bytes).
- `branch`: optional; the remote's default branch when absent.
- `global`: optional; honoured for admins only, others always import a private project.
- Refused with "Code projects are disabled." when `[code_projects] enabled` is off, and with
  "Importing projects is restricted to administrators." for a non-admin when
  `[code_projects] import_user_required` is `"admin"`.

Response `{ok: true, message: "Checking repository '<name>'…"}` means accepted for checking: the
server then checks the repository exists before adding anything. If it does, the project appears
(`code_project_status_changed`) and is cloned and indexed; if not, `code_project_import_failed`
goes to the user. `ok: false` with "A repository URL is required." or "Import rejected (invalid
name/URL, duplicate, or blocked host)."

#### `code_projects_link`
Index an existing checkout on the server, in place. **Admin only**, whatever
`import_user_required` says. DAWN never changes its branch or deletes its files.
```json
{ "type": "code_projects_link", "payload": { "path": "/srv/src/example-repo", "name": "example-repo" } }
```
- `path`: required; a git checkout under one of `[code_projects] allowed_local_roots`.
- `name`: optional; defaults to the path's last component.

Response `{ok: true, message: "Local repository linked; indexing…"}`, or `ok: false` with "Code
projects are disabled.", "Linking a local repository is restricted to administrators.", "A local
path is required." or "Link rejected (path not allowed, not a git repo, or duplicate name)."

#### `code_projects_refresh`
Fetch the latest commits (a clone) and re-index what changed.
```json
{ "type": "code_projects_refresh", "payload": { "name": "example-repo" } }
```
Response `code_projects_action_response`: `ok: true`, "Re-index queued.", or `ok: false` with
"Refresh failed." or one of the shared refusals below.

#### `code_projects_rebuild`
Fetch (a clone), discard the project's code graph and index it again from scratch.
```json
{ "type": "code_projects_rebuild", "payload": { "name": "example-repo" } }
```
Response `code_projects_action_response`: "Clean rebuild queued (may take a few minutes).", or
`ok: false` with "Rebuild failed." or a shared refusal.

#### `code_projects_set_branch`
Switch a clone to another branch and rebuild its index.
```json
{ "type": "code_projects_set_branch", "payload": { "name": "example-repo", "branch": "develop" } }
```
Response `code_projects_action_response`: "Branch set; rebuilding index (may take a few
minutes).", or `ok: false` with "A branch name is required.", "Set branch failed (linked repos
track their live checkout)." (a linked project, or a branch name git doesn't accept) or a shared
refusal.

#### `code_projects_delete`
Remove a project: its code graph, its row and, for a clone, DAWN's clone on disk (a linked
checkout's files are never touched).
```json
{ "type": "code_projects_delete", "payload": { "name": "example-repo" } }
```
Response `code_projects_action_response`: "Project deleted.", or `ok: false` with "Delete
failed." or a shared refusal. `code_project_status_changed` follows for the removed id.

Shared refusals of refresh, rebuild, set_branch and delete (`ok: false`): "A project name is
required.", "No such project.", "You can only manage projects you imported.". These act on a
project by its `name`, not its `id`, and they don't check `[code_projects] enabled`.

#### `code_project_status_changed` (push)
A project was added, changed state or was deleted.
```json
{ "type": "code_project_status_changed", "payload": { "project_id": 5 } }
```
Sent to every logged-in connection of every user (satellites included), whoever owns the
project: it carries only the id, and each client refetches its own `code_projects_list`.

#### `code_project_import_failed` (push)
An accepted `code_projects_import` (or `code_projects_link`) was dropped before the project was
added: the repository wasn't found or reachable (an import), or another project took the name
meanwhile. Sent to the
user who asked, every connection.
```json
{ "type": "code_project_import_failed", "payload": { "name": "example-repo", "reason": "Repository not found or unreachable." } }
```
`reason` is a sentence for people.

---

### Music Streaming

Music messages are accessible to both authenticated WebUI users and registered
satellites.

The audio itself goes over the separate music socket (`music_port`), which
authenticates with `{"type":"auth","token":<session token>}`. A browser's music
socket must also carry the login cookie of the login that owns that session (its
upgrade URL names the app, `?app=<app>`, as every request does) (a
browser sends it: the cookie is same-site whatever the port); otherwise it gets
`auth_failed`. When the session is destroyed (logout, expiry) the server closes the
music socket.

The music socket's replies are bare objects, not `payload`-wrapped: on success
`{"type":"auth_ok"}`, then the audio starts; a refusal (an unknown token, a session that isn't
this login's, too many music connections) is `{"type":"auth_failed","reason":"invalid_token"}`
and the socket is closed. An `auth` message without a `token` gets no reply. After `auth_ok`
the only message the client sends is its buffer report (`music_buffer`, below); anything else
is ignored.

#### `music_buffer` (music socket)
After `auth_ok`, the only message a client sends on the music socket: how much music it has
buffered and not yet played, so the server can pace the stream. Top-level fields, no `payload`:
```json
{ "type": "music_buffer", "buffered_ms": 1840 }
```
- `buffered_ms`: decoded audio waiting to play plus frames waiting to decode, in milliseconds.
  Values below 0 count as 0, above 10000 as 10000.
- Send it often (the WebUI sends one with every playback ring report, about every 32 ms) and
  `0` at once after a flush (a seek, a skip, a stop), so the server refills quickly.
- The server keeps the client about 2 seconds ahead while reports are fresh, and bursts to
  refill after a drain. With no report for 1 second (or a client that never sends one) it falls
  back to sending at real time.
- No reply. Anything else the client sends on the socket after auth, malformed or binary
  included, is ignored and never closes the socket.

#### `music_subscribe`
Subscribe to music streaming for this connection.
```json
{
   "type": "music_subscribe",
   "payload": {
      "quality": "high",
      "bitrate_mode": "vbr"
   }
}
```
Response: `music_state` (current playback state)

#### `music_unsubscribe`
Stop receiving music audio for this connection.
```json
{"type": "music_unsubscribe"}
```

#### `music_control`
Control music playback.
```json
{
   "type": "music_control",
   "payload": {
      "action": "play|pause|stop|next|previous|seek",
      "position_sec": 30.0
   }
}
```
- `action`: `play`, `pause`, `stop`, `next`, `previous`, `seek`, `play_index`, `add_to_queue`,
  `remove_from_queue`, `clear_queue`, `add_artist`, `add_album`, `toggle_shuffle`, `cycle_repeat`, `volume`
- `position_sec`: only for `seek` action
- `play` behaviour depends on the payload and current state:
  - with `path` or `query` — plays that specific track / search hit (adds to top of queue)
  - bare (no `path`/`query`) while **paused** — resumes the pause
  - bare while **stopped** with a non-empty queue — starts the current `queue_index` track
    (index clamped into range); empty queue just re-echoes state
  - bare while **already playing** — re-echoes state (no-op)
- Response: `music_state` (updated state)
- `add_album` appends a whole album, in track order: `album_key` (from an `albums` browse)
  plus `album` and `artist`, as the browse sent them. Keys change when the library is rescanned
  or Plex syncs (each restart), so the server checks a key against `album` and otherwise looks
  the album up by `album` and `artist`; a client may send those alone.

#### `music_search`
Search the music library.
```json
{
   "type": "music_search",
   "payload": {
      "query": "bohemian rhapsody"
   }
}
```
Response: `music_search_response`

#### `music_library`
Browse the music library (artists, albums, tracks).
```json
{
   "type": "music_library",
   "payload": {
      "type": "stats|tracks|artists|albums|tracks_by_artist|tracks_by_album",
      "artist": "Queen",
      "album": "A Night at the Opera"
   }
}
```
Response: `music_library_response`

Albums:
- `type: "albums"` (with `offset`/`limit`) lists albums: an album artist plus an album name,
  editions merged, with one entry for all its copies (formats, Plex). Each entry carries `key`
  (a string id, valid until the next library rescan or Plex sync), `name`, `artist` and
  `track_count`; `total_count` counts albums.
- `type: "tracks_by_album"` returns an album's tracks in order (disc, track number), up to the
  queue's size: `album_key`, or `album` (and optional `artist`) as for `add_album`.

#### `music_queue`
Manage the playback queue.
```json
{
   "type": "music_queue",
   "payload": {
      "action": "list|add|clear|remove",
      "path": "/path/to/song.flac",
      "index": 0
   }
}
```
Response: `music_queue_response` for `list`; `music_state` for `add`, `clear` and `remove`.

---

### Scheduler

#### `scheduler_action`
List, dismiss, snooze, cancel and edit the user's scheduled events (alarms, timers, reminders,
tasks, briefings), and clear missed ones. One message type; `action` picks the verb.
```json
{
   "type": "scheduler_action",
   "payload": {
      "action": "snooze",
      "event_id": 42,
      "snooze_minutes": 10
   }
}
```
Sent by browsers and by satellites (a satellite needs no login). A request without a `payload` gets
no reply. Failures come as an `error` frame (`code`, `message`), not as a response; only `list`
has a response. The others answer with broadcasts: `scheduler_events_changed` (the queue
changed: refetch with `list`) and, for a dismiss or snooze, `scheduler_notification`. Both go
to every connection of the event's owner, satellites included, the sender among them.

| `action` | Fields | Does | On success |
|---|---|---|---|
| `list` | — | The user's events | `scheduler_events_response` (below) |
| `dismiss` | `event_id` | Stops a ringing timer, alarm or reminder; a repeating one gets its next occurrence | `scheduler_notification` (`dismissed`), `scheduler_events_changed` |
| `snooze` | `event_id`, `snooze_minutes` | Snoozes a ringing timer, alarm or reminder | `scheduler_notification` (`snoozed`, message "Snoozed for N minutes", or "Snoozed for 1 minute"), `scheduler_events_changed` |
| `cancel` | `event_id` | Cancels a pending or snoozed event; a repeating one's series ends | `scheduler_events_changed` |
| `cancel_occurrence` | `event_id` | Cancels this occurrence only; a repeating event's next occurrence is scheduled | `scheduler_events_changed` |
| `clear_missed` | `event_id` | Clears an event from the missed list | `scheduler_events_changed` |
| `update` | `event_id`, `instructions` | Sets a briefing's summarization instructions | `scheduler_events_changed` |
| `dismiss_missed` | `missed_notif_id`, `event_id` (optional) | Deletes a missed-notification replay (see `scheduler_notification`); with `event_id`, also dismisses that event if it is the user's and still ringing | nothing; with a still-ringing `event_id`, the same broadcasts as `dismiss` |

- `event_id`: the event's id (from `list` or a notification); required (positive) for every
  action but `list` and `dismiss_missed`.
- `snooze_minutes` (optional): `0` or absent uses `[scheduler] default_snooze_minutes`; at most
  120. An event already snoozed `[scheduler] max_snooze_count` times is dismissed instead (no
  error).
- `instructions`: a string, required (an empty string clears them); cut to 1023 bytes on a
  character boundary. Only a briefing that hasn't fired or been cancelled can be edited.
- `update` needs a browser login (a satellite gets `UNAUTHORIZED`). `dismiss_missed` needs a
  user: a browser's login, or the user a satellite is assigned to.

Who may act on an event: `dismiss` and `snooze` take any event from a satellite (it can stop
what it announced), and only the user's own from a browser. Every other action takes only the
user's own events, from either.

Errors (`code`: `message`):
- every action: `INVALID_PARAM`: "Missing action" / "Missing or invalid event_id" / "Unknown
  scheduler action"
- `dismiss`, `snooze`: `NOT_FOUND`: "Event not found"; `FORBIDDEN`: "Not your event";
  `PROCESSING_ERROR`: "Could not dismiss" (the event is still ringing: a running task or
  briefing ends itself and can't be dismissed); `NOT_FOUND`: "No ringing event to snooze". A
  dismiss of an event that already stopped ringing is not an error: the server re-sends its
  `scheduler_notification` so other clients close their banner.
- `cancel`, `cancel_occurrence`, `clear_missed`, `update`: `NOT_FOUND`: "Event not found" (also
  for another user's event); `ALREADY_RESOLVED`: "Event already fired or cancelled" / "Event was
  not in 'missed' state" / "Briefing can no longer be edited (it already fired or was
  cancelled)"
- `update`: `INVALID_PARAM`: "Missing instructions string" / "Instructions can only be set on a
  briefing (this is a timer)"; `INTERNAL_ERROR`: "Could not update briefing instructions"
- `dismiss_missed`: `INVALID_PARAM`: "Missing missed_notif_id"; `FORBIDDEN`: "Not authenticated"

##### `scheduler_events_response`
The reply to `list`: the user's active events (pending, snoozed, ringing; soonest first, at most
50), then their missed ones (most recent first, at most 50). A satellite with no user gets an
empty list.
```json
{
   "type": "scheduler_events_response",
   "payload": {
      "success": true,
      "events": [
         {
            "id": 42,
            "event_type": "briefing",
            "status": "pending",
            "name": "Morning briefing",
            "message": "",
            "fire_at": 1791702000,
            "effective_fire_at": 1791702000,
            "created_at": 1791648000,
            "duration_sec": 0,
            "snoozed_until": 0,
            "snooze_count": 0,
            "say_aloud": 1,
            "deliver_to": "",
            "instructions": "Keep it short.",
            "recurrence": "weekdays",
            "recurrence_days": "",
            "original_time": "07:00",
            "source_location": "",
            "tool_name": "",
            "tool_action": "",
            "tool_value": "",
            "steps": [
               { "tool_name": "weather", "tool_action": "forecast", "tool_value": "" }
            ]
         }
      ]
   }
}
```
| Field | Meaning |
|---|---|
| `event_type` | `timer`, `alarm`, `reminder`, `task` or `briefing` |
| `status` | `pending`, `ringing`, `snoozed` or `missed` (the list holds no others) |
| `fire_at`, `created_at`, `snoozed_until` | Unix seconds; `snoozed_until` is 0 unless snoozed |
| `effective_fire_at` | When it fires next: `snoozed_until` when snoozed, else `fire_at`. Sort on this |
| `duration_sec` | A timer's length |
| `snooze_count` | Times snoozed |
| `say_aloud` | 1 when it is spoken when it fires |
| `deliver_to` | Where a briefing's output goes (a messaging channel), or `""` |
| `instructions` | A briefing's summarization instructions |
| `recurrence` | `once`, `daily`, `weekdays`, `weekends`, `weekly` or `custom`; `recurrence_days` for `custom` |
| `original_time` | The time of day as it was asked for |
| `source_location` | Where it was set |
| `tool_name`, `tool_action`, `tool_value` | A task's tool call; a `tool_value` over 203 bytes is cut to at most 200 bytes plus `...` |
| `steps` | Only on a briefing with steps (at most 8): its tool calls in order, each `tool_value` cut as above |

---

### Watches (Authenticated)

The proactive-attention watches panel: rules that watch a metric (suit, STAT, network and
component telemetry) and alert the user when it crosses a line, changes too fast, or goes
silent. The `attention` tool edits the same watches by voice, so a watch made either way is
editable from both. Every verb acts on the logged-in user's watches only. Replies are
`{success, error?}`; a request without a `payload` gets no reply, except `watch_list` and
`watch_readings_subscribe`. A user has at most 64 watches. Edits aren't pushed to the user's
other tabs: ask `watch_list` again.

Alerts themselves arrive as `attention_alert`; live values as `watch_readings`. These are not
the Stocks Panel's ticker watchlist (`stocks_watch_*`), nor the `stocks` tool's `watch_add` /
`watch_remove` actions.

#### `watch_list`
The user's watches, the metric catalog, and whether proactive attention is on. No payload.
```json
{ "type": "watch_list" }
```
Response:
```json
{
   "type": "watch_list_response",
   "payload": {
      "success": true,
      "attention_enabled": true,
      "watches": [
         {
            "id": 12,
            "name": "CO2 above 1500",
            "named": false,
            "metric": "suit.co2_ppm",
            "label": "CO2",
            "unit": "ppm",
            "rule_type": "threshold",
            "direction": "above",
            "threshold": 1500,
            "absence_after_sec": 0,
            "notify": "alert",
            "enabled": true,
            "source": "",
            "has_current": true,
            "current": 612,
            "breaching": false
         }
      ],
      "catalog": [
         {
            "key": "suit.co2_ppm",
            "label": "CO2",
            "unit": "ppm",
            "rule_type": "threshold",
            "default_direction": "above",
            "default_threshold": 1500
         }
      ]
   }
}
```
- `attention_enabled`: the `[attention] enabled` switch. While it is off no watch fires and no
  `watch_readings` are sent; say so rather than show live-looking watches.
- A watch:

| Field | Meaning |
|---|---|
| `id` | The watch, for the other verbs |
| `name`, `named` | Its name; `named` is true for a name the user chose (kept, and spoken in alerts). A system name follows the condition and changes when it is edited |
| `metric`, `label`, `unit` | The catalog metric it watches |
| `rule_type` | `threshold` (a level), `slope` (a rate), `absence` (the feed went silent) or `match` |
| `direction` | `above` / `below` for `threshold`, `rising` / `falling` for `slope` |
| `threshold` | The level; absent on a `slope` watch |
| `slope_per_min`, `slope_window_sec` | Only on a `slope` watch: the rate (units per minute, positive) and the window it is measured over |
| `absence_after_sec` | Seconds of silence before an `absence` watch fires |
| `notify` | `alert`, `ambient` or `digest` |
| `enabled` | Off: kept but never fires |
| `source` | `seed` for a watch DAWN created by default, otherwise `""` |
| `has_current`, `current` | Whether the metric has a value now, and the value (absent when none) |
| `breaching` | Whether the condition holds now (the same state the readings stream carries) |

- `catalog`: every metric a watch can use, for an add form. `rule_type` and
  `default_direction` are the metric's defaults; `default_threshold` its default level.

Error: "Failed to list watches".

#### `watch_add`
Add a watch on a catalog metric, starting from the metric's defaults.
```json
{
   "type": "watch_add",
   "payload": { "metric": "suit.co2_ppm", "direction": "above", "threshold": 1200, "notify": "alert" }
}
```
- `metric` (required): a catalog `key`.
- Optional, each as in `watch_update`: `name`, `rule_type`, `direction`, `threshold`,
  `slope_per_min`, `slope_window_sec`, `notify`.
- An add whose condition is identical to an existing watch's updates that watch (and enables
  it) instead of adding a second one.

Response: `watch_add_response` (`success`; no id: ask `watch_list`). Errors: "Missing metric",
"Unknown metric", "A watch with that name already exists", "Couldn't save the watch (you may be
at the watch limit)", and the condition errors listed under `watch_update`.

#### `watch_update`
Change a watch. Only the fields sent change.
```json
{
   "type": "watch_update",
   "payload": { "id": 12, "rule_type": "slope", "direction": "rising", "slope_per_min": 50 }
}
```
- `id` (required, positive).
- `name`: a name of the user's own (control characters removed, trailing spaces trimmed, at most
  63 bytes); unique among the user's watches, ignoring case. An empty name leaves the name as
  it is.
- `rule_type`: `threshold` or `slope` switches a numeric watch between the two (the direction
  defaults to `above` or `rising` when the current one doesn't fit). Any other value is
  ignored. An `absence` or `match` watch can't change kind.
- `direction`: `above`, `below`, `rising`, `falling` (any case); anything else leaves it.
- `threshold`: the level. On an `absence` watch it sets `absence_after_sec` instead (seconds,
  clamped to 0..604800).
- `slope_per_min` (positive) and `slope_window_sec` (positive, at most 604800): a `slope`
  watch's rate and window. Zero or negative values are ignored.
- `notify`: `alert`, `ambient` or `digest`; anything else leaves it.

Response: `watch_update_response`. Errors: "Missing watch id", "Watch not found", "This
metric's rule kind can't be changed", "A rising/falling watch needs a rate (slope_per_min > 0)",
"A rising/falling watch needs direction 'rising' or 'falling'", "An above/below watch needs
direction 'above' or 'below'", "A watch with that name already exists", "Couldn't update the
watch".

#### `watch_set_enabled`
Turn a watch on or off.
```json
{ "type": "watch_set_enabled", "payload": { "id": 12, "enabled": false } }
```
- `id` (required); `enabled` (optional, default `true`).

Response: `watch_set_enabled_response`. Errors: "Missing watch id", "Couldn't update the watch"
(also for an id that isn't the user's).

#### `watch_remove`
Delete a watch.
```json
{ "type": "watch_remove", "payload": { "id": 12 } }
```
Response: `watch_remove_response`. Errors: "Missing watch id", "Couldn't remove the watch".

#### `watch_readings_subscribe`
Start or stop `watch_readings` for this connection. Subscribe while the panel is shown and
unsubscribe when it is hidden; a new connection starts unsubscribed.
```json
{ "type": "watch_readings_subscribe", "payload": { "enabled": true } }
```
- `enabled`: `true` subscribes, `false` unsubscribes. **Absent (or no payload) unsubscribes.**

Response: `watch_readings_subscribe_response` (`success: true`, always).

---

### Calendar

Built only with the calendar tool (`DAWN_ENABLE_CALENDAR_TOOL`); a daemon without it answers
every `calendar_*` type with `UNKNOWN_TYPE`. Every request needs a login (else an `error` frame
`UNAUTHORIZED`) and acts only on the user's own CalDAV accounts: an account or calendar of
another user answers like a missing one (except `calendar_toggle_calendar`, which answers
"Access denied"). Not admin-only. Every reply is `<request>_response`
with `{success, error?, …}`; `error` is a sentence for people, not a code. Every request below
except `calendar_list_accounts`, `calendar_list_my_calendars` and `calendar_upcoming_events`
needs a `payload`: a request without one gets no reply.

#### Account management

##### `calendar_list_accounts`
The user's CalDAV accounts (at most 32). No payload.
```json
{ "type": "calendar_list_accounts" }
```
Response:
```json
{
   "type": "calendar_list_accounts_response",
   "payload": {
      "success": true,
      "accounts": [
         {
            "id": 3, "name": "Personal", "caldav_url": "https://caldav.example.com/",
            "username": "alice", "has_password": true, "auth_type": "basic",
            "oauth_account_key": "", "enabled": true, "read_only": false,
            "last_sync": 1785000000,
            "principal_url": "https://caldav.example.com/principals/alice/",
            "calendar_home_url": "https://caldav.example.com/calendars/alice/"
         }
      ]
   }
}
```
- `auth_type`: `"basic"` (username + password) or `"oauth"` (a linked Google account, named by
  `oauth_account_key`; see Google account linking).
- `has_password`: a password is stored. The password itself never leaves the daemon.
- `last_sync`: epoch seconds, `0` before the first sync. `principal_url` /
  `calendar_home_url`: what discovery found, `""` before it ran.

##### `calendar_add_account`
```json
{
   "type": "calendar_add_account",
   "payload": {
      "name": "Personal",
      "caldav_url": "https://caldav.example.com/",
      "username": "alice",
      "password": "app-password-here",
      "read_only": false
   }
}
```
- `name`, `caldav_url`: required (stored up to 127 and 511 bytes).
- `auth_type`: optional; `"oauth"`, or anything else (default) for a username and password.
- `username`, `password`: required unless `auth_type` is `"oauth"`.
- `oauth_account_key`: for `"oauth"`, the `account_key` from `oauth_exchange_code`. For a Google
  account the browser sends `caldav_url`
  `https://apidata.googleusercontent.com/caldav/v2/<account email>/`.
- `read_only`: optional, default false. A read-only account is never written to.
- A `password` member is refused when the WebUI isn't served over HTTPS (`[webui] https`):
  "Cannot send password over unencrypted connection. Enable HTTPS in webui configuration."

Response: `{"success": true}`, or `success: false` with `error` "Missing required fields: …",
"Account already exists" (the same OAuth key, or the same username and URL, for this user) or
"Failed to add account". The reply carries no `id`: refetch with `calendar_list_accounts`.

The reply is sent after the server has run discovery on the new account (a network round trip
on the socket's thread). When discovery works its calendars are stored and a first sync is
queued, and `calendar_sync_done` follows when it ends. When discovery fails the account is
still added (`success: true`) with no calendars; `calendar_test_account` discovers them later.

##### `calendar_edit_account`
Change an account's name, URL, username and, optionally, password.
```json
{
   "type": "calendar_edit_account",
   "payload": {
      "id": 3, "name": "Personal", "caldav_url": "https://caldav.example.com/",
      "username": "alice", "password": ""
   }
}
```
- `id`, `name`, `caldav_url`, `username`: all required, an OAuth account's included (send its
  current `username`, `""` if it has none).
- `password`: optional; a non-empty one replaces the stored password, an empty one keeps it.
  Any `password` member, even an empty one, is refused when the WebUI isn't on HTTPS.
- `auth_type`, `oauth_account_key`, `read_only` and `enabled` aren't changed here.

Response: `{"success": true}`, or `success: false` with `error` "Missing required fields: id,
name, caldav_url, username", "Account not found or access denied", "Failed to encrypt
password" or "Failed to update account". An edit doesn't re-run discovery or a sync.

##### `calendar_remove_account`
```json
{ "type": "calendar_remove_account", "payload": { "id": 3 } }
```
Deletes the account with its calendars and cached events. For an OAuth account, the Google
grant is also revoked and its tokens deleted, unless one of the user's email accounts uses the
same `oauth_account_key`. Response: `{"success": true}`, or `success: false` with "Missing
account id", "Account not found or access denied" or "Failed to remove account".

##### `calendar_test_account`
Log in and run discovery: stores the principal and calendar-home URLs and adds any calendar not
seen before (a new calendar starts active unless the server marks it read-only, such as a
subscribed feed; existing ones keep their setting).
```json
{ "type": "calendar_test_account", "payload": { "id": 3 } }
```
Response:
```json
{
   "type": "calendar_test_account_response",
   "payload": { "success": true, "message": "Connection successful, calendars discovered" }
}
```
or `success: false` with "Missing account id", "Account not found or access denied" or
"Connection test failed — check URL, username, and password". The reply waits for the server's
answers. It doesn't sync events: send `calendar_sync_account` for that.

##### `calendar_sync_account`
Ask for a sync now.
```json
{ "type": "calendar_sync_account", "payload": { "id": 3 } }
```
Response `{"success": true, "queued": true, "id": 3}` at once; the sync runs on the server's
sync thread and `calendar_sync_done` reports how it ended (and `calendar_events_changed`
follows when it changed anything). On failure `success: false`, `id`, and `error` "Missing
account id" (no `id` then), "Account not found or access denied" or "Sync could not start".

##### `calendar_list_calendars`
One account's calendars (at most 32), active or not.
```json
{ "type": "calendar_list_calendars", "payload": { "account_id": 3 } }
```
Response:
```json
{
   "type": "calendar_list_calendars_response",
   "payload": {
      "success": true,
      "calendars": [
         { "id": 7, "display_name": "Work", "color": "#3b82f6", "is_active": true,
           "caldav_path": "/calendars/alice/work/" }
      ]
   }
}
```
or `success: false` with "Missing account_id", "Account not found or access denied" or
"Failed to list calendars". Note the request member is `account_id`, not `id`.

##### `calendar_toggle_calendar`
Turn one calendar on or off. Only active calendars are synced, searched by the model and
returned by `calendar_upcoming_events` / `calendar_list_my_calendars`.
```json
{ "type": "calendar_toggle_calendar", "payload": { "id": 7, "is_active": false } }
```
- `id`: the calendar's id (from `calendar_list_calendars`), not the account's.

Response: `{"success": true}`, or `success: false` with "Missing id or is_active", "Calendar not
found", "Access denied" (another user's calendar) or "Failed to toggle calendar". No push follows:
refetch the panel's events.

##### `calendar_toggle_read_only`
```json
{ "type": "calendar_toggle_read_only", "payload": { "id": 3, "read_only": true } }
```
Response `{"success": true, "read_only": true}`: `read_only` is the stored value read back, not
the request echoed. On failure `success: false` with "Missing id or read_only", "Account not
found or access denied" or "Failed to update read-only flag".

##### `calendar_set_enabled`
Turn a whole account on or off; a disabled account isn't synced or used.
```json
{ "type": "calendar_set_enabled", "payload": { "id": 3, "enabled": false } }
```
Response: `{"success": true}`, or `success: false` with "Missing id or enabled", "Account not
found or access denied" or "Failed to update enabled flag".

#### Calendar data

##### `calendar_list_my_calendars`
List the user's active calendars flat across all accounts — the `calendar_id`→{name, color}
map a panel needs to group/color the events returned by `calendar_upcoming_events`, in one
call (instead of `calendar_list_accounts` + per-account `calendar_list_calendars`). No payload.
```json
{ "type": "calendar_list_my_calendars" }
```
Response `calendar_list_my_calendars_response`:
```json
{
   "type": "calendar_list_my_calendars_response",
   "payload": {
      "success": true,
      "calendars": [
         { "id": 7, "account_id": 3, "name": "Work", "color": "#3b82f6" }
      ]
   }
}
```
- `id` matches the per-event `calendar_id` from `calendar_upcoming_events`.
- Active calendars only — exactly the set the pull draws events from.
- Re-run on `calendar_events_changed` (a newly-synced calendar can appear).

##### `calendar_upcoming_events`
Read a window of upcoming occurrences from the offline cache (build a calendar panel).
Authenticated; each user sees only their own accounts' events.
```json
{
   "type": "calendar_upcoming_events",
   "payload": {
      "days": 7,
      "calendar_name": "Work"
   }
}
```
- **Window** — two mutually-exclusive forms:
  - `days` (convenience) — `now` .. `now + days*86400`. Default 7, clamped 1–90.
  - `start` + `end` (epoch seconds, power path) — used when **both** present; `start < end`,
    span ≤ 366 days. Providing exactly one of `start`/`end`, or `start >= end`, is an error.
- `calendar_name`: optional case-insensitive filter; omitted/empty = all active calendars.
- Includes both timed **and all-day** occurrences, ordered by start; capped at 256 (the
  farthest are dropped and `truncated:true` is set).
- Reads the pre-expanded SQLite cache — no network at request time.
- Payload is optional (a bare request defaults to a 7-day window).
- Response: `calendar_upcoming_events_response`:
```json
{
   "type": "calendar_upcoming_events_response",
   "payload": {
      "success": true,
      "start": 1784949199,
      "end": 1785554000,
      "truncated": false,
      "events": [
         {
            "id": 412, "calendar_id": 7, "uid": "abc@google.com",
            "summary": "Standup", "location": "",
            "start": 1784971800, "end": 1784973600,
            "all_day": false, "start_date": "", "end_date": "",
            "cancelled": false, "is_override": false
         }
      ]
   }
}
```
  - `calendar_id`: the owning calendar (grouping/coloring key). Map to name/color via
    `calendar_list_calendars`; the response does not carry the calendar *name* (it would
    force an extra join, and the id is the stable key).
  - `start_date`/`end_date`: `YYYY-MM-DD`, only meaningful when `all_day` is true.
  - `start`/`end` (top level): echo the resolved window.

---

### Google account linking (OAuth)

The Google sign-in that calendar and email accounts use. Built when the calendar or the email
tool is (`DAWN_ENABLE_CALENDAR_TOOL` or `DAWN_ENABLE_EMAIL_TOOL`); a daemon with neither answers
these with `UNKNOWN_TYPE`. Every request needs a login (else `UNAUTHORIZED`) and a `payload`
(a request without one gets no reply), and acts on the logged-in user's own Google tokens.
Google is the only provider: a `provider` member, which the browser sends, is ignored. The
server needs Google OAuth credentials in `secrets.toml`; without them the replies say "Google
OAuth not configured".

The flow:
1. Optionally `oauth_check_scopes`: if a linked account already has the scopes, skip to step 4
   with its `account_key`.
2. `oauth_get_auth_url`, then open the returned `url` (the browser uses a popup). Google
   redirects to the daemon's `/oauth/callback` page (the redirect URL configured in
   `secrets.toml`, by default `https://localhost:<port>/oauth/callback`), which posts the
   `code` and `state` back to the window that opened it (`postMessage`, same origin only).
3. `oauth_exchange_code` with that `code` and `state`: the tokens are stored, encrypted.
4. Create the account that uses them: `calendar_add_account` or `email_add_account` with
   `auth_type: "oauth"` and `oauth_account_key` set to the `account_key`.

#### `oauth_get_auth_url`
```json
{
   "type": "oauth_get_auth_url",
   "payload": { "scopes": "https://www.googleapis.com/auth/calendar" }
}
```
- `scopes`: required; space-separated, each starting `https://www.googleapis.com/auth/` or
  `https://mail.google.com/` (read up to 1023 bytes). The server always adds
  `https://www.googleapis.com/auth/userinfo.email`, and adds every scope already granted to
  the user's linked Google accounts, so authorizing a second service doesn't drop the first's.

Response:
```json
{
   "type": "oauth_get_auth_url_response",
   "payload": { "success": true, "url": "https://accounts.google.com/o/oauth2/v2/auth?client_id=…&state=…" }
}
```
or `success: false` with `error` "Missing OAuth scopes", "Invalid OAuth scopes", "Google OAuth
not configured" or "Failed to generate auth URL". The last also means the user already has two
sign-ins pending (the server keeps at most two per user and eight in all). A pending sign-in
lasts 5 minutes; its `state` is single-use and only the user who asked for it can exchange it.

#### `oauth_exchange_code`
```json
{ "type": "oauth_exchange_code", "payload": { "code": "4/0Example-code", "state": "example-state" } }
```
Both required. Response:
```json
{
   "type": "oauth_exchange_code_response",
   "payload": {
      "success": true,
      "account_key": "alice@example.com",
      "scopes": "https://www.googleapis.com/auth/calendar https://www.googleapis.com/auth/userinfo.email"
   }
}
```
- `account_key`: the Google account's email, or `google_<user id>` when Google didn't return
  one. Linking the same Google account again replaces its stored tokens.
- `scopes`: what Google granted, space-separated.

On failure `success: false` with "Missing code or state", "Google OAuth not configured", "Token
exchange failed" (an unknown, expired or used `state`, or Google refused the code) or "Failed to
store tokens". No calendar or email account is created here.

#### `oauth_disconnect`
Revoke a linked Google account at Google and delete its tokens.
```json
{ "type": "oauth_disconnect", "payload": { "account_key": "alice@example.com" } }
```
Response `{"success": true}` whenever the request is well formed and Google OAuth is configured:
the tokens are deleted locally even when revoking at Google fails. Otherwise `success: false`
with "Missing account_key" or "Google OAuth not configured". It doesn't check whether a calendar
or email account still uses the key; those accounts stop working until relinked. Removing the
account itself (`calendar_remove_account`) is the usual way out: it revokes the grant only when
no other account uses it.

#### `oauth_check_scopes`
Whether one of the user's linked Google accounts already has every scope asked for.
```json
{ "type": "oauth_check_scopes", "payload": { "scopes": "https://www.googleapis.com/auth/calendar" } }
```
Response:
```json
{
   "type": "oauth_check_scopes_response",
   "payload": { "success": true, "has_scopes": true, "account_key": "alice@example.com" }
}
```
- `has_scopes: true`: `account_key` is the first linked account (of at most 8 looked at) that
  has them all. An empty `scopes` string counts as covered by any linked account.
- `has_scopes: false`: `account_key`, when present, is the user's first linked account (for a
  scope upgrade through `oauth_get_auth_url`); absent when the user has none.
- `success: false` with "Missing scopes" when `scopes` is absent.

---

### Email

Every email verb is per user (the logged-in connection's account) and may carry `req`: a
string of at most 64 bytes with no control characters, echoed in its reply's payload, so a
client can match replies to requests. A `req` that's longer, has a control character or
isn't a string is ignored (the reply carries none). Every reply is `<verb>_response` with `{success, error_code?, error?, req?, …}`.
`error_code` is one of the names below; `error` is a sentence for people.

| `error_code` | Meaning |
|---|---|
| `INVALID_REQUEST` | A member is missing, of the wrong type, or past a limit |
| `UNAVAILABLE` | Email is turned off |
| `NO_ACCOUNT` / `ACCOUNT_NOT_FOUND` | The user has no enabled account / not one with that id |
| `AUTH_FAILED` / `AUTH_REVOKED` | The server refused the login / the OAuth grant was revoked (reconnect) |
| `UNREACHABLE` / `TIMEOUT` / `RATE_LIMITED` | Network, server or provider trouble; try again |
| `NOT_FOUND` | No such message (an undo: it's no longer where the move put it, e.g. Trash was emptied) |
| `READ_ONLY` | The account is read-only in DAWN: it can't trash, archive or undo |
| `NO_TRASH` / `FOLDER_MISSING` | The account has no Trash / the folder (an Archive, or the one an undo goes back to) doesn't exist |
| `IN_TRASH` | Archive of a message in Trash or Spam (Gmail): restore it first |
| `NOT_REMOVED` | Copied to Trash or Archive, but the original couldn't be removed. Don't retry (it would copy again); the next list shows both |
| `OUTCOME_UNKNOWN` | The server didn't say whether the move happened; the panel reloads on the `email_changed` that follows (`refresh`) |
| `UNDO_EXPIRED` | The undo token is unknown, past its 60 s window, already used or running, or the account changed (server, login, read-only, disabled) |
| `UNSUPPORTED_QUERY` | This account can't run the search (IMAP: non-ASCII text) |
| `CURSOR_STALE` | The cursor no longer applies: start again without one |
| `BUSY` | Too many email requests at once (per session or per user); try again shortly |
| `SUPERSEDED` | A newer request of the same kind replaced this one before it finished: ignore it |
| `SHUTTING_DOWN` | The daemon is stopping |
| `CANCELLED` / `FAILED` | Stopped / anything else |

Every request gets exactly one reply, even when it's refused or replaced, with two
exceptions: an archive, trash or undo that already started when the daemon stops finishes on
the server without a reply (`email_changed` tells every tab what it did), and when a socket's
session passes to another user (log out, log in), the old user's requests get none.
Email work runs off the socket's thread, so replies may come back in a different order than
the requests went out: match on `req`.

#### Account settings

`email_list_accounts` (payload optional, `{req?}`) returns `accounts: [{id, name, imap_server,
imap_port, imap_ssl, smtp_server, smtp_port, smtp_ssl, username, display_name, has_password,
auth_type, oauth_account_key, enabled, read_only, max_recent, max_body_chars, digest_depth,
can_trash?, can_archive?}]` and `limits: {max_recent_max, max_recent_default,
digest_depth_max, digest_depth_default, body_chars_min, body_chars_max}`. Passwords never
leave the daemon. `can_trash` / `can_archive`: whether the account has a Trash / an Archive to
move to. Always true on Gmail; on IMAP present only once DAWN has learned it (from a move, or
the first inbox page of `email_list`). Absent means not known yet, not false.

Every account-settings request below may carry `req`, echoed like the panel's.

| Request | Payload | Notes |
|---|---|---|
| `email_add_account` | `{name, imap_server, smtp_server, username, password?, auth_type?, imap_port?, imap_ssl?, smtp_port?, smtp_ssl?, display_name?, read_only?, oauth_account_key?}` | `auth_type` `"app_password"` (needs `password`) or `"oauth"` (needs `oauth_account_key`) |
| `email_update_account` | `{id, …any of the above, max_recent?, max_body_chars?, digest_depth?}` | An out-of-range number is refused, not dropped. Clears what's known of `can_trash` / `can_archive` |
| `email_remove_account` | `{id}` | |
| `email_set_read_only` | `{id, read_only}` | A read-only account can't send, trash or archive |
| `email_set_enabled` | `{id, enabled}` | |
| `email_test_connection` | `{id, req?}` | Runs the IMAP and SMTP logins: `{imap_ok, smtp_ok, imap_busy?, error_code?}`. `imap_busy`: IMAP wasn't tested because the account was in use |

#### The mail panel (flag `email_client`)

Limits: `account_ids` 1–16 distinct ids; `message_ids` 1–50; a message id under 192 bytes;
`query` 1–256 bytes; `limit` 1–50 (default 25); `cursor` up to 16 KB. Text members may not
contain control characters, and a message id must have the shape of its account's ids. Past a
limit, or with an id of the wrong shape, the request is refused with `INVALID_REQUEST`.

**A row** (`email_list`, `email_search`):

```json
{"account_id": 3, "message_id": "INBOX:4211", "thread_id": "18c2…", "from_name": "Ann",
 "from_addr": "ann@example.com", "subject": "Lunch", "date": 1759831200, "preview": "",
 "unread": true, "starred": false, "important": true}
```

`date` is the server's receive time in epoch seconds. `thread_id`, `starred` and
`important` are there only where the backend has them (Gmail). `preview` is the
provider's snippet, and `""` on IMAP accounts. A `message_id` is opaque: pass it back as is.
IMAP ids carry the mailbox's epoch (`INBOX:4211.7`, folder, UID, UIDVALIDITY), so an id
from before the mailbox was rebuilt answers `NOT_FOUND` instead of naming another message;
an id without it (`INBOX:4211`, from older clients) still works, unpinned. A message's id
changes when it moves: use the one a reply or `email_changed` gives.

##### `email_list` / `email_search`

```json
{"type": "email_list", "payload": {"account_ids": [3, 5], "folder": "inbox",
 "unread_only": false, "limit": 25, "cursor": null, "req": "l-17"}}
{"type": "email_search", "payload": {"query": "invoice", "unread_only": false, "limit": 25,
 "req": "s-4"}}
```

Omitted `account_ids` means every enabled account. `folder` (list only) defaults to
`"inbox"`; `sent`, `trash`, `spam`, `drafts`, `starred`, `important`, `all` and a label or
folder name work too. Search looks through all mail where the backend has it: Gmail (without
Spam and Trash), and IMAP servers that mark an All Mail folder (`\All`); on other IMAP servers,
the inbox. (If the server's folder list can't be read, it's asked again after 5 minutes;
meanwhile the inbox is searched.)

```json
{"type": "email_list_response", "payload": {"success": true, "req": "l-17",
 "rows": [ … ], "cursor": "eyJ2Ijox…",
 "partial": [{"account_id": 5, "error_code": "AUTH_FAILED", "error": "The server refused the login"}],
 "accounts": [{"account_id": 3, "inbox_unread": 12, "status": "ok"},
              {"account_id": 5, "inbox_unread": null, "status": "auth_failed"}]}}
```

- **Rows** come newest first across the accounts. Within an account they keep its own
  order (IMAP: by UID), so the merged list can be slightly out of date order. Each page
  resumes exactly where the last one stopped for every account. A Gmail account resumes by
  date, so mail arriving or leaving between pages doesn't shift it (no row twice, none
  skipped; a page can change `limit`). Gmail pages that come back with nothing to show but
  more after them don't end the list.
- **`cursor`**: send it back unchanged, with the same other members, for the next page;
  `null` means there is no more. It's opaque. A cursor presented with a different filter
  (accounts, folder, query, `unread_only`), naming an account that's gone or not the
  user's, or one the server no longer accepts (a rebuilt IMAP mailbox, or an `all` that now
  reads a different folder than when the cursor was made) is refused with `CURSOR_STALE`:
  start again without one. A page can hold fewer than `limit` rows while
  `cursor` is still set.
- **`partial`**: accounts that failed this page, and ids asked for that aren't the user's
  enabled accounts (`ACCOUNT_NOT_FOUND`). The other accounts still return rows. A failed
  account keeps its place in `cursor`, so a load-more tries it again (the cursor isn't null
  while one is left); rows from an account that recovers after a failure may arrive out of
  date order with the ones already shown.
- **`accounts`** (first page of `email_list` for the inbox only, no cursor): each account's
  inbox unread count, on the same login as the list. `inbox_unread` is `null` when it isn't
  known. `status`: `ok`, `auth_revoked`, `auth_failed` or `unreachable` (timeouts and rate
  limits count as unreachable). `can_trash` / `can_archive` as in `email_list_accounts`
  (present once known; this page learns them for an IMAP account, at most once an hour).

A newer `email_list` or `email_search` from the same session replaces one still running
(that one is answered `SUPERSEDED`).

##### `email_read`

```json
{"type": "email_read", "payload": {"account_id": 3, "message_id": "INBOX:4211",
 "mark_read": true, "req": "r-9"}}
{"type": "email_read_response", "payload": {"success": true, "req": "r-9", "unread": false,
 "message": {"account_id": 3, "message_id": "INBOX:4211", "subject": "Lunch",
   "from_name": "Ann", "from_addr": "ann@example.com",
   "to": [{"name": "", "addr": "me@example.com"}], "cc": [], "to_total": 40,
   "reply_to": {"name": "", "addr": "list@example.com"}, "date": 1759831200,
   "body_text": "…", "body_html": "<p>…</p>", "text_truncated": false,
   "html_truncated": false,
   "attachments": [{"part_id": "2", "filename": "menu.pdf", "mime": "application/pdf",
                    "size": 48211, "inline": false}]}}}
```

- `to_total` / `cc_total` appear when the list was cut at 32; `reply_to` when the message
  has one; `content_id` on an attachment when it has one; `attachments_truncated` when
  more than 16 exist.
- `body_text` is at most 256 KB. `body_html` is the decoded HTML, at most 1 MB, and cut
  further (with `html_truncated`) so the whole frame stays within 1.5 MB.
- **`body_html` is the sender's HTML, unsanitized. It must never reach `innerHTML`, or any
  other HTML sink, in any DAWN client.** Render it only in a frame sandboxed *without*
  `allow-same-origin` and *without* `allow-scripts`, or sanitize it (e.g. DOMPurify) before
  any HTML sink. Aurora is served same-origin with DAWN under `/aurora`: HTML that escapes
  runs with the user's DAWN session and WebSocket.
- **Read state.** `mark_read` (default `true`) leaves the message read. `false` leaves it as
  it was: on IMAP the fetch itself marks the message read, so DAWN marks it unread again
  right after (even if the read failed partway); another client may see it read for that
  moment. `unread` is the message's state after the call, as it really ended up.
- A newer `email_read` from the same session replaces one still waiting (not one already
  running).

##### `email_set_flags`

```json
{"type": "email_set_flags", "payload": {"account_id": 3, "message_ids": ["INBOX:4211"],
 "unread": true, "req": "f-2"}}
{"type": "email_set_flags_response", "payload": {"success": true, "req": "f-2",
 "updated": ["INBOX:4211"], "failed": []}}
```

Marks messages read (`unread: false`) or unread. Only the read state ever changes, so it
works on read-only accounts. `failed: [{message_id, error_code, error}]`: `NOT_FOUND` for a
message that isn't there (on Gmail a batch the provider accepts reports every id updated).
Repeating it is harmless. On IMAP the messages may span at most 8 folders per call.

##### `email_archive` / `email_trash`

```json
{"type": "email_trash", "payload": {"account_id": 3,
 "message_ids": ["INBOX:4211.7", "INBOX:4212.7"], "req": "t-5"}}
{"type": "email_trash_response", "payload": {"success": true, "req": "t-5",
 "done": [{"message_id": "INBOX:4211.7", "undo": "9f2c…(32 hex)"},
          {"message_id": "INBOX:4212.7", "undo": null}],
 "failed": []}}
```

Moves messages of one account (1–50) to its Trash or Archive. Gmail: trash is the Trash label,
archive takes the message out of the inbox. IMAP: the folder the server marks `\Trash` /
`\Archive` (or a usual name; on Gmail over IMAP, All Mail), never a delete. Refused with
`READ_ONLY` on a read-only account.

- **`done`**: every message now in Trash or Archive, including ones already there.
  `message_id` is the id as the server knows it (pinned). `undo` is a token for `email_undo`,
  good for 60 seconds, or `null` when the move can't be undone (already there, or the server
  can't say where it went: IMAP without UIDPLUS). `left_flagged: true` (IMAP without MOVE or
  UIDPLUS): the message was copied and marked deleted, and is removed at the folder's next
  expunge; it may still show in other clients until then.
- **`failed`**: `[{message_id (as sent), error_code, error}]`. `NOT_REMOVED` and
  `OUTCOME_UNKNOWN` mean don't retry.
- On IMAP the messages may span at most 8 folders per call. When nothing moved because the
  whole call failed (the login, no Trash, the account busy), the reply is an error reply with
  that `error_code`; otherwise it's `done` and `failed`, even if the call stopped partway.

**Queueing.** Archive, trash and undo share one queue per session: one runs, up to 3 wait
in order behind it, none is ever replaced; a 5th is `BUSY`. Only a move that would run
at once counts toward the user's limit of live email requests (and is `BUSY` past it); one
that waits behind another doesn't. A move that started isn't cancelled
midway: its login and its first folder (IMAP) or message (Gmail) always run, and a stop
between folders leaves the rest where they were. So a started move always moves something
and always sends `email_changed`; a stop waits at most for that first folder or message
(bounded by the server timeouts). When the daemon stops, a queued one that never started is
answered `SHUTTING_DOWN`; one that started may get no reply, so its ids are not reported
either way: wait for `email_changed`, which says what moved.
Clicks are meant to be batched client-side (a short debounce, then one call with every id).

##### `email_undo`

```json
{"type": "email_undo", "payload": {"account_id": 3, "undo": ["9f2c…"], "req": "u-1"}}
{"type": "email_undo_response", "payload": {"success": true, "req": "u-1",
 "restored": [{"undo": "9f2c…", "row": {"account_id": 3, "message_id": "INBOX:4390.7", …}}],
 "failed": []}}
```

Moves messages back where a move took them (1–50 tokens, each 32 lowercase hex characters,
no repeats, all from moves on `account_id`). `row` is the message where it is now (a new id
on IMAP), or `null` when it came back but couldn't be read (the `email_changed` that follows
then asks for a reload). `failed: [{undo, error_code, error}]`: `UNDO_EXPIRED`, `NOT_FOUND`
(no longer in Trash or Archive), `FOLDER_MISSING` (the folder it came from is gone). A token
that failed for a passing reason (busy, network) can be sent again within its window.

##### `email_changed` (push)

```json
{"type": "email_changed", "payload": {"account_id": 3, "state": null, "kind": "trash",
 "undo": false, "created": [], "updated": [], "destroyed": ["INBOX:4211.7"]}}
```

Sent to every browser tab of the user after any archive, trash or undo on one of their
accounts (from the panel or from Friday), including moves whose reply didn't arrive.
`kind`: `"trash"` or `"archive"`; `undo`: whether it was an undo. `created`: rows that
appeared (an undo's restored messages; a row replaces any with the same id). `destroyed`:
ids that left their folder: an IMAP id names its folder; a Gmail id left the inbox (archive)
or every view but Trash (trash), and for a Gmail undo the `created` row replaces it.
`updated` is empty and `state` is `null` for now. `refresh: true` (only when set): reload
this account's lists, because a row couldn't be read or a move may have happened without its
answer.

##### `email_unread_counts`

```json
{"type": "email_unread_counts", "payload": {"req": "c-1"}}
{"type": "email_unread_counts_response", "payload": {"success": true, "req": "c-1",
 "accounts": [{"account_id": 3, "inbox_unread": 12, "status": "ok"}]}}
```

Every enabled account's inbox unread count, with the same `status` values as the list's
`accounts`. For refreshing: the first page of `email_list` for the inbox already carries
them.

##### Email in chat (`email_refs`)

A `text` frame may name one email the user attached (flag `email_refs`):

```json
"email_refs": [{"account_id": 3, "message_id": "u4182.1700000000",
                "from": "Bob <bob@example.com>", "subject": "Lunch Friday?"}]
```

- `account_id` — the user's own enabled account (an id from `email_list` rows); `message_id`
  — the message's id exactly as `email_list`/`email_read` give it (a Gmail id, or an IMAP
  `folder:uid.uidvalidity`; at most 191 bytes, no control characters). `from` and `subject` — optional, what the chip shows; stored for
  display only (cut to 160 bytes, control characters as spaces) and never sent to the model.
- The words are still required: a frame with `email_refs` and no text gets `EMPTY_MESSAGE`.
- More than one ref: `EMAIL_REF_LIMIT`. Anything else wrong (not a list of objects, a bad id,
  an account that isn't the user's or is disabled, two accounts the model couldn't tell apart
  by name, email off, or no conversation could be created for the turn): `EMAIL_UNAVAILABLE`.
  Either way one `error` frame (with `client_ref`), the turn doesn't run and no message is
  saved. (A conversation created for the turn, below, stays even if the turn is then refused
  for a full queue, `TURN_QUEUE_FULL`.)
  The account is checked in DAWN's database; nothing is fetched before the turn runs.
- The model reads the email itself with the email tool, so its text reaches the model as a
  tool result framed as someone else's (never as the user's words), and is kept with the
  conversation like any tool result. If the message is gone, the model says so.
- In that turn no confirm counts (an email the user attached could say "yes"), and the model
  can't make device changes or take other actions; it can read, look things up and prepare
  (a draft, say), and the user's next message confirms as usual.
- A turn with an email always has a conversation: when none is open, the daemon creates one
  first and sends `new_conversation_response`, as for a voice turn.
- The question row stores `email_ref` (`account_id`, `message_id`, `from`, `subject`) and
  sends it on `load_conversation_response` rows, `message_appended` and
  `get_memory_fact_source_response` messages, and in exports.

### Stocks Panel

A live view of the logged-in user's Charles Schwab portfolio, and a watchlist of tickers they
don't hold. Built only with the Schwab tool (`DAWN_ENABLE_SCHWAB_TOOL`); a daemon without it
answers these types with `UNKNOWN_TYPE`. No feature flag. Every request needs a login (else
`UNAUTHORIZED`); each user sees only their own Schwab link, and a user with none gets
`status: "not_connected"` on the portfolio. The watchlist is not a link check: an empty
watchlist makes no Schwab request and answers `status: "ok"` with `symbols: []`, linked or not.
Satellites never receive the pushes. (This stock watchlist, `stocks_watch_*`, is not the
proactive-attention watches of `watch_list` / `watch_add`; see Watches.)

The server keeps one snapshot per user and pushes it to every subscribed connection of that user;
requests never fetch from Schwab themselves. A fetch happens when a connection of the user is
subscribed and either nothing is cached yet or the regular session is open and the snapshot is
older than 30 s, at most once per 30 s. Outside regular hours a panel therefore shows the snapshot
of its last fetch, labelled by `as_of`. The portfolio and the watchlist are separate
subscriptions with separate caches.

**Lifecycle.** Subscriptions belong to the socket and aren't stored: a new or reconnected socket
starts unsubscribed, so send the subscribe again after `init` / `reconnect`. Subscribe while the
panel is visible and unsubscribe when it's hidden. When the user has nothing cached yet, subscribe
sends no frame at once; the first update follows the fetch it triggers. When none of a user's
connections is subscribed, their cache is dropped at the refresher's next pass (within 30 s);
a subscribe after that starts with a fresh fetch.

**`status`** (both updates):

| `status` | Meaning | Numbers in the frame |
|---|---|---|
| `ok` | The last fetch succeeded | Yes |
| `rate_limited` | Schwab answered 429; no retry for 60 s, doubling to 10 min | Last-known, if any |
| `error` | Network, Schwab or parse failure; no retry for 60 s | Last-known, if any |
| `not_connected` | The user has no Schwab link; no retry for 5 min | Last-known, if any |
| `token_expired` | The Schwab login expired (weekly); relink with `dawn-admin schwab auth`; no retry for 5 min | Never |

The intervals are the earliest a retry can happen, not a schedule: once one has passed, the next
fetch still needs the rule above (nothing cached, or the regular session open). So with a snapshot
cached, a failure outside regular hours, or a relink then, is not retried until the session
opens. A watchlist edit is the exception: it cancels any interval running, and its fetch
happens at once, whatever the hour. If that fetch fails, it is retried when its own interval has
passed, again whatever the hour, until one succeeds.

Last-known numbers keep their original `as_of`, so show them dimmed with "as of …" rather than
blanking. Before the first fetch completes, a frame built for the user says `not_connected` with no
numbers.

**`market`** (both updates): `regular`, `pre` (07:00–09:30 ET), `post` (the close to 20:00 ET; the
close is 13:00 on NYSE half-days) or `closed` (nights, weekends, NYSE holidays). Worked out when
the frame is built, not when the numbers were fetched.

**`ext {price, change, change_pct}`**, on a position or watchlist row, is the extended-hours
price, per share, and its move against the regular-session close. Present only when one of the
user's subscribed connections asked for `ext_hours` (it then reaches all of them), `market` is `pre` or `post`, the snapshot was fetched in a pre or post window (not necessarily the current one; check `as_of`), and
the symbol has an extended-hours trade; feature-detect on its presence. On the portfolio, `value`
and `day_change` already reflect the extended-hours price after the close (Schwab's own
valuation), so `ext.change` is the after-hours move alone; multiply by `qty` for a position's
dollar figure.

Percent fields (`*_pct`) are percentages (`18.82` is +18.82 %), signed. Day change is against the
prior close and unrealized P/L against cost; account and total percents use the summed position
values, not the account value (which includes cash). Amounts are numbers in dollars; format them
client-side.

#### `stocks_portfolio_subscribe`
Start the portfolio push for this socket.
```json
{ "type": "stocks_portfolio_subscribe", "payload": { "ext_hours": true } }
```
- `ext_hours` (bool, optional, default false): include the extended-hours overlay in pre/post.
  Sending subscribe again replaces it.
- Replies with the cached `stocks_portfolio_update` when the user has one, and wakes the
  refresher.

#### `stocks_portfolio_unsubscribe`
Stop the portfolio push for this socket. No payload, no reply.

#### `stocks_portfolio_get`
Send the cached `stocks_portfolio_update` to this socket once.
```json
{ "type": "stocks_portfolio_get" }
```
- Sends nothing when the user has nothing cached, and causes a fetch only while one of the user's
  connections is subscribed. A panel should subscribe rather than poll with this.
- `ext_hours` is accepted and ignored: the overlay follows the subscriptions.

#### `stocks_portfolio_update` (push)
Sent to every subscribed connection of the user after each fetch (success or failure), and as the
reply to subscribe and get.
```json
{
   "type": "stocks_portfolio_update",
   "payload": {
      "status": "ok",
      "market": "regular",
      "ext_hours": false,
      "as_of": 1790000000,
      "link_expires_at": 1790400000,
      "total_value": 100000.00,
      "total_day_change": -250.00,
      "total_day_change_pct": -0.31,
      "total_unrealized_pl": 12000.00,
      "total_unrealized_pl_pct": 17.65,
      "accounts": [
         {
            "label": "…1234",
            "acct_type": "MARGIN",
            "value": 100000.00,
            "cash": 20000.00,
            "day_change": -250.00,
            "unrealized_pl": 12000.00,
            "unrealized_pl_pct": 17.65,
            "positions": [
               {
                  "symbol": "XYZ",
                  "description": "EXAMPLE CORP",
                  "asset_type": "equity",
                  "qty": 100,
                  "price": 800.00,
                  "value": 80000.00,
                  "unrealized_pl": 12000.00,
                  "unrealized_pl_pct": 17.65,
                  "day_change": -250.00,
                  "day_change_pct": -0.31
               }
            ]
         }
      ]
   }
}
```

| Field | Type | Meaning |
|---|---|---|
| `status` | string | See the table above |
| `market` | string | See above |
| `ext_hours` | bool | Whether this snapshot was fetched with the extended-hours overlay (in a frame without numbers, the opt-in of the last fetch) |
| `as_of` | integer | Epoch seconds when the numbers were fetched; `0` in a frame without numbers |
| `link_expires_at` | integer, optional | Epoch seconds when the Schwab link will need relinking (link time + 7 days), so a panel can warn ahead; on every frame, `token_expired` included, when known |
| `total_value` | number | Sum of the accounts' `value` |
| `total_day_change`, `total_day_change_pct` | number | Sum of every position's day change, and its percent |
| `total_unrealized_pl`, `total_unrealized_pl_pct` | number | Sum of every position's unrealized P/L (long positions), and its percent |
| `accounts` | array | One per Schwab account |

`total_*` and `accounts` are present only in a frame with numbers (see `status`); test for
`accounts`.

Account:

| Field | Type | Meaning |
|---|---|---|
| `label` | string | The account number masked to its last four digits (`…1234`); the full number is never sent |
| `acct_type` | string | Schwab's account type as reported (e.g. `MARGIN`, `CASH`) |
| `value` | number | Schwab's liquidation value: the account total, cash included. Rows reconcile to it |
| `cash` | number | Cash balance |
| `day_change` | number | Sum of the account's positions' day change (no account-level percent is sent) |
| `unrealized_pl`, `unrealized_pl_pct` | number | Sum of the positions' unrealized P/L, and its percent of their cost |
| `positions` | array | Every holding, never filtered |

Position:

| Field | Type | Meaning |
|---|---|---|
| `symbol` | string | Ticker, or an OCC option symbol (up to 21 characters, with spaces) |
| `description` | string, optional | Company or fund name; absent when Schwab gives none. Schwab cuts names at about 20 characters, upper case |
| `asset_type` | string | `equity`, `etf`, `option`, `mutual_fund`, `cash_equivalent` or `other`. Schwab reports most ETFs as equities |
| `qty` | number | Net quantity (long minus short); fractional or negative possible |
| `price` | number | Price per unit held (`value / qty`): per share, or per contract for an option, sent so the client needn't divide and round |
| `value` | number | Market value |
| `unrealized_pl`, `unrealized_pl_pct` | number | Unrealized P/L on the long position, and its percent of cost |
| `day_change`, `day_change_pct` | number | The position's change today (a total, not per share), and its percent of the prior close |
| `ext` | object, optional | Extended-hours overlay, per share (see above) |

#### `stocks_watch_subscribe`
Start the watchlist push for this socket.
```json
{ "type": "stocks_watch_subscribe", "payload": { "ext_hours": true } }
```
- `ext_hours` (bool, optional, default false), as for the portfolio.
- Replies with the cached `stocks_watch_update` when the user has one, and wakes the refresher.

#### `stocks_watch_unsubscribe`
Stop the watchlist push for this socket. No payload, no reply.

#### `stocks_watch_get`
Send the cached `stocks_watch_update` to this socket once. No payload. Same limits as
`stocks_portfolio_get`.

#### `stocks_watch_set`
Add tickers to the user's watchlist and remove others.
```json
{ "type": "stocks_watch_set", "payload": { "add": ["xyz", "ABC"], "remove": ["OLD"] } }
```
- `add`, `remove` (arrays of strings, optional): at most the first 25 entries of each are read.
  Symbols are upper-cased; only `A–Z`, `0–9` and `.`, at most 23 characters.
- Subscribes this socket to the watchlist push.
- Removes apply at once; removing a symbol that isn't there does nothing.
- Adds are checked with Schwab first, off the socket's thread, and only symbols Schwab doesn't
  reject are stored. The `stocks_watch_set_response` comes when that's done and goes to **every**
  watch-subscribed connection of the user (the sender included); a `stocks_watch_update` with the
  new list follows. If Schwab can't be reached, well-formed symbols are stored unchecked.
- When no add was queued (only removes, or adds already waiting from an earlier request), the
  response comes at once to the sender only, and an update follows.
- The list holds at most 25 symbols. The `stocks` tool's `watch_add` / `watch_remove` edit the same
  list, and a voice edit pushes a `stocks_watch_update` to subscribed panels.

#### `stocks_watch_set_response`
```json
{
   "type": "stocks_watch_set_response",
   "payload": {
      "success": true,
      "added": ["XYZ"],
      "rejected": [ { "symbol": "ABC", "reason": "unknown" } ]
   }
}
```
- `success` is always true; per-symbol outcomes are in the arrays.
- `added`: symbols now on the list, upper-cased, including ones that already were.
- `rejected[].reason`: `invalid` (characters or length not allowed, or it couldn't be stored;
  `symbol` is then as sent for a malformed symbol, upper-cased when it could not be stored), `unknown` (Schwab says it isn't a ticker), `limit` (the list already
  holds 25), `busy` (the server couldn't take the request just then; try again).

#### `stocks_watch_update` (push)
Sent to every watch-subscribed connection of the user after each watchlist fetch (success or
failure; an edit triggers one at once), and as the reply to subscribe and get.
```json
{
   "type": "stocks_watch_update",
   "payload": {
      "status": "ok",
      "market": "post",
      "as_of": 1790000000,
      "symbols": [
         { "symbol": "XYZ", "description": "EXAMPLE CORP", "asset_type": "equity",
           "price": 50.00, "day_change": 1.25, "day_change_pct": 2.56,
           "ext": { "price": 50.40, "change": 0.40, "change_pct": 0.80 } },
         { "symbol": "QQQX", "quotable": false }
      ]
   }
}
```
- `as_of`: epoch seconds of the last successful fetch; `0` before the first.
- `symbols` is always present: the whole stored list in the order it was added, after a
  successful fetch; the last-known rows after a failure; empty for `token_expired` or before the
  first fetch. No `ext_hours` echo and no `link_expires_at` here.
- A row Schwab returned: `symbol`, `description` (optional, as for positions), `asset_type`,
  `price` (last), `day_change` and `day_change_pct` (**per share**, against the prior close,
  unlike a position's total), and `ext` (optional).
- A stored symbol a successful fetch didn't return: `{symbol, quotable: false}` only. Show it dimmed
  and keep its remove control. `quotable` is absent on every other row (absent means quotable). A
  valid symbol outside market hours still comes back, with its last price.

### DAP2 Satellite Messages

These messages are only accepted from satellite connections (identified by
the `satellite_register` handshake).

#### `satellite_register`
Register a satellite device with the daemon.
```json
{
   "type": "satellite_register",
   "payload": {
      "uuid": "550e8400-e29b-41d4-a716-446655440000",
      "name": "Kitchen Assistant",
      "location": "Kitchen",
      "tier": 1,
      "capabilities": {
         "local_asr": true,
         "local_tts": true,
         "wake_word": true,
         "music": true
      },
      "reconnect_secret": "prev_secret_here"
   }
}
```
- `uuid`: 36-char UUID (8-4-4-4-12 hex format), required
- `name`: Display name (default: "Satellite")
- `location`: Room/location string
- `tier`: 1 (text-first, local ASR/TTS) or 2 (audio path, server ASR/TTS)
- `capabilities`: Tier 2 must NOT claim `local_asr` or `local_tts`. `music` is false
  for a Tier 1 that can't play streamed music (built without Opus); the music tool
  then refuses there. Absent means true (older firmware)
- `reconnect_secret`: Provided on reconnection to reclaim previous session
- Response: `satellite_register_ack`

#### `satellite_query`
Send a transcribed text query to the AI (Tier 1 satellites).
```json
{
   "type": "satellite_query",
   "payload": {
      "text": "What's the weather like?"
   }
}
```
- Response: streaming via `stream_start` → `stream_delta` → `stream_end`,
  then `state` changes, and/or `transcript` with role `satellite_response`

#### `satellite_ping`
Application-level keepalive (every 10 seconds).
```json
{"type": "satellite_ping"}
```
Response: `satellite_pong`

#### `volume_state`
A satellite reports its playback volume, in answer to `volume_set` (Tier 1 sends it after
applying one). Satellite connections only; no response.
```json
{"type": "volume_state", "payload": {"level": 65}}
```
`level`: 0–100 (clamped). The `volume` tool's `get` reports it.

#### `ping`
Application-level liveness probe for **browser (WebUI)** clients — the
authenticated counterpart to `satellite_ping`. Sent on an idle-gated heartbeat
(~10 s) so a client can detect a stale/timed-out session.
```json
{"type": "ping", "payload": {"seq": 42}}
```
- `seq` (optional): correlation token echoed verbatim in the `pong` so the
  client can match replies and discard stale ones.

Response: `pong` — but **only** for a valid, authenticated session
(`conn_require_auth`). A revoked/expired/unauthenticated connection receives an
`UNAUTHORIZED` `error` and no `pong`; that absence is the client's staleness
signal. When the login can't be checked (the database is down), it receives
`SERVICE_UNAVAILABLE` and no `pong`, and stays logged in. The reply carries `{seq?, server_time_ms,
expires_at}` (the shape of [`satellite_pong`](#satellite_pong) plus the login's expiry; see How long
a login lasts).

#### OTA (over-the-air updates)
Server→satellite firmware updates. Control plane is on this WebSocket; the image
itself is pulled over HTTPS. The signing key never touches the daemon — devices
verify a signed manifest against a baked-in public keyring. See `docs/OTA_DESIGN.md`.

The `satellite_register` payload advertises OTA support + the running version:
```json
{ "firmware_version": "2.0.0", "capabilities": { "ota": true } }
```
The daemon never sends `ota_offer` to a device that didn't advertise `ota: true`.

**`ota_offer`** (server → device) — sent when an operator pushes an update. The
manifest + signature are inlined (tiny); only the image is fetched over HTTPS.
```json
{
   "type": "ota_offer",
   "payload": {
      "platform": "rpi", "version": "2.1.0",
      "url_path": "/api/ota/rpi/2.1.0/image",
      "token": "<one-time hex>", "image_size": 756696,
      "sha256": "<hex>", "manifest": "<hex>", "sig": "<hex>",
      "allow_downgrade": false
   }
}
```
Device flow: verify `sig` over `manifest` (Ed25519, baked keyring) → check
`abi_tag` in the manifest matches its OS/ABI → `GET <url_path>?uuid=<uuid>&token=<token>`
(TLS required; token is one-time, uuid+version-bound) → verify image SHA-256 →
apply → reboot → reconnect reporting the new `firmware_version` (the daemon then
commits success — a device never self-declares success).

**`ota_ack`** / **`ota_reject`** (device → server):
```json
{"type": "ota_ack", "payload": {"version": "2.1.0"}}
{"type": "ota_reject", "payload": {"reason": "abi_mismatch"}}
```

**`ota_status`** (device → server) — progress; `state` ∈ downloading | verifying |
applying | rebooting | failed:
```json
{"type": "ota_status", "payload": {"state": "downloading", "error": null}}
```
A `failed` status releases the server-side single-flight lock so a re-push is allowed.

---

## Server → Client Messages

### Background-Job Observe Stream (Phase 2)

The attach/replay contract that makes a background job watchable from any client,
browser or not. Reference consumer: `tests/tools/tail_conversation.py`.

**Attach ordering is part of the contract**, not an implementation detail — a
client that simply appends what it receives must end up with a coherent view:

1. `load_conversation_response` — message history
2. `conversation_events` — durable step log, `seq > last_seq`
3. `stream_resume` — in-memory partial of a turn still in flight (if any)
4. then live `conversation_event` / `message_appended`

#### `conversation_events`
Durable replay batch, ASC by `seq`. `seq` is **per-conversation** monotonic, so a
client keeps one cursor per conversation. A `payload` of `null` means the body was
aged out by retention (`[jobs] event_retention_days`) while the step itself was
kept — render it as "expired", not as empty.
```json
{
   "type": "conversation_events",
   "payload": {
      "conversation_id": 1006,
      "events": [
         { "seq": 1, "kind": "status", "payload": "{\"state\":\"generating\"}", "created_at": 1784949199 },
         { "seq": 2, "kind": "tool_call", "payload": "{\"tool\":\"search\",\"args\":{...}}", "created_at": 1784949199 }
      ],
      "has_more": false,
      "last_seq": 21
   }
}
```

#### `conversation_event`
One step, pushed live. Same shape as an entry above, plus `conversation_id`.
Carries the DB-assigned `seq` so a client can **dedup against the replay batch** —
a live frame can race an in-flight attach.
`kind` ∈ `status` | `tool_call` | `tool_result` | `spawn` | `complete`
(`terminal_chunk` is reserved for Phase 4). `payload` is an opaque **string** of
pre-redacted JSON; render it as text only (§8.7) — it can carry web/tool output.

#### `message_appended`
A persisted assistant message **with its body**. Distinct from
`conversation_messages_appended`, which is signal-only ("refetch") and useless to a
client that cannot issue REST calls. Without this frame an event-only consumer
would watch a job run and never learn its answer.
```json
{
   "type": "message_appended",
   "payload": { "conversation_id": 1006, "message_id": 21547, "role": "assistant", "text": "..." }
}
```
The answer reaches a client by **either** route: this frame (turn completes while
attached) or the message batch on attach (already-finished job). `complete` carries
`final_message_id` to correlate the two.

`stream_id` (unsigned integer) is the stream the reply was delivered on; 0 = none (a user
message, a research report). It counts per session, so two sessions of one user on the same
conversation can both have a stream N. `reasoning` (string) carries the reply's reasoning, and is
present only when there is some. `session_id` (unsigned integer, the value the `session`
frame sends) names the session whose turn wrote the row. It is present if and only if a non-job
session's turn wrote the row (a typed or spoken WebUI message and its reply, a job result
re-engaging a conversation a browser is viewing), and absent for a background job's answer (its
stream reaches every viewer of the job), a detached re-engagement and a research report. A
client that streamed a reply adopts `message_id` onto its bubble when `conversation_id` and
`stream_id` match and, if `session_id` is present, it is the session the client was on when it
streamed; a row naming another session is that session's reply and is drawn as one. With
`session_id` absent, match on the first two alone.

A user message sent from the WebUI is fanned out the same way (`"role": "user"`, its saved
text) to every browser of its user. The copy sent to the connection that sent it also carries
`client_ref` when the `text` frame had one (flag `turn_refs`), so that connection can match it
to its turn even if its `transcript` echo was dropped. Other connections' copies don't carry it.
A user message that asked about an attached email carries `email_ref` in every copy, the
sender's included (its `transcript` echo doesn't: the sender shows the chip it attached).

A reply the user stopped while it streamed (flag `stopped_replies_saved`) carries
`stopped: true`, and `text` is what was saved of it. The sender adopts it like any reply and
replaces its bubble's text with `text`: the bubble may hold a little more (text drawn after the
saved point) or tags the saved row doesn't.

### Background-Job List Frames (Phase 2)

Job state reaches a client through three frames **split by object lifetime**. The
split is load-bearing, not cosmetic: a user's **active** jobs are structurally
bounded (`max_active_jobs` clamps to 256 and gates every reservation), so the
active set arrives whole and may be counted; **history** is unbounded, so it is
paginated and a page of it must never be counted.

All three carry the same job object:
```json
{
   "conversation_id": 1006, "parent_id": 990, "title": "research X",
   "status": "running", "spawn_mode": "detached", "on_complete": "notify",
   "spawn_depth": 1, "reinvoke_count": 0,
   "created_at": 1784949199, "started_at": 1784949200, "finished_at": 0,
   "deliver_to": "telegram-main", "error": "..."
}
```
`deliver_to` and `error` are **omitted when empty**. `kind` is present (`"coding"`)
only on a coding job, whose open requests `coding_pending` fetches. `status` ∈ `queued` |
`running` | `done` | `failed` | `interrupted` | `cancelled`; the first two are the
active states — treat anything else as terminal so an unrecognized future status
cannot pin a row in a client's active set forever.

#### `jobs_snapshot` — the complete active set
Reply to `jobs_request`; sent on (re)connect. Replaces the client's whole active
set. `truncated: true` means the server hit its row ceiling and any count derived
from this set is a **lower bound** — it should not happen under any valid config.
```json
{ "type": "jobs_snapshot", "payload": { "success": true, "jobs": [ /* … */ ], "truncated": false } }
```
When the server can't read the set (a database or memory failure) it still answers,
with `success: false`, an `error` sentence and no `jobs`:
```json
{ "type": "jobs_snapshot", "payload": { "success": false, "error": "Couldn't read your background jobs." } }
```
Keep the active set the client already has (replacing it with nothing would hide
running jobs) and tell the user its counts may be stale.

#### `job_update` — one job's lifecycle transition
Pushed when a job enters the active set (spawn), changes state within it
(`queued`→`running`), or leaves it (any terminal disposition, including the boot
interrupted-scan). Clients **upsert by `conversation_id` and drop on terminal
status** — set membership, never +/-1 arithmetic, so duplicate or out-of-order
frames converge.
```json
{ "type": "job_update", "payload": { "job": { /* … */ } } }
```
The job object carries **`resumed: true`** on exactly one transition: a successful
`job_action{resume}`, which moves a job *backwards* out of a terminal state. Since
clients treat terminal as a sink — that is what stops a stale frame from
resurrecting a finished job — this flag is what tells them to release the mark. A
tab that did not initiate the resume sees only this frame, so the signal has to be
in the data rather than in frame ordering.

#### `coding_pending_response`
```json
{
   "type": "coding_pending_response",
   "payload": {
      "jobs": [
         {
            "job": 1977,
            "agent": "Claude Code",
            "model": "claude-sonnet-5",
            "requests": [
               { "request_id": 12, "kind": "permission", "line": "reach the host `downloads.arduino.cc`", "repo": "dawn" },
               { "request_id": 13, "kind": "question", "line": "", "question": "Which branch?\nOptions: main / dev" }
            ]
         }
      ]
   }
}
```
`line` is DAWN's checked wording (empty when DAWN can't show what is asked: offer Deny
only); `repo` is present when allowing on the repo can be offered; `question` is the
agent's question, its own words (show it as text, as theirs). A job with nothing
open has `requests: []`. `agent` names what runs the job; `model` is the model its last
run reported, absent before the first run (show "default"). A coding job's work happens
in its agent, not on the conversation's model, so a client shows these in place of its
model controls and doesn't send text into the job's conversation (follow-up work goes
through the assistant: "follow up on job 1977: ...").

#### `coding_view_response`
`ok` with `result`, the gateway's answer as is, or `error`. For `changes`: `head`,
`base`, `task_id` (the job's branch is `refs/agent/<task_id>` in the gateway's
mirror), `files` ({`path`, `added`, `removed`, `to_check`, `why`}; `added`/`removed` null
for a binary file; `to_check` true for a file to check before publishing, with its pattern's
`why` when it has one; those first, at most 2000) with `files_total`, and `commits`
({`hash`, `subject`, `merge`}, at most 100, always the whole job's), `base_name` (the
base branch), `scope` (as applied: `all`, `since_push` or the full hash),
`scope_commits` (hashes it covers), `scope_merge` (a merge commit's scope) with
`merge_recorded` (its files are the conflicts the gateway recorded; false: an
approximation, the files it changed beyond both sides), `since_push_available` (bool),
`nothing_since_push` (asked for `since_push` with nothing since the push: shows all), and on a `since_push` file `includes_main` (main's
changes from a merge are in its diff), plus `job_state`, DAWN's facts about the job (the
viewer's Publish and Close buttons come only from these): `status`, `closed` (bool),
`pr_url` (checked; absent when none), `unpublished_commits` (int, `-1` when unknown) and
`published` (bool). For `file`: `path`, `view`, and `text` (a unified
diff, or the file), or `too_large` (bytes), `binary` or `missing`. Paths, subjects
and text are the agent's: show them as text, and show invisible and
direction-changing characters (bidi controls, zero-width characters) visibly, since
this is where the user checks the work before publishing it.
```json
{ "type": "coding_view_response", "payload": { "job": 42, "what": "file", "nonce": "v3", "ok": true, "result": { "path": "src/a.c", "view": "diff", "text": "@@ -1 +1 @@\n-a\n+b\n" } } }
```

#### `coding_publish_preview_response`
`ok` with `preview`, or `error`. `preview`: `head` (full hash), `repo`, `branch`,
`base`, `commits` (count listed) with `commits_more` (true: 100 or more), `files` (count
listed) with `files_total`, `to_check` ({`path`, `why`}: files to check before publishing,
`why` empty for none), `history_only` ({`path`, `to_check`, `why?`}, the gateway's as it is:
paths changed by the job's commits but not in its final diff, public in the branch's
history), `workflows` (true: the work changes `.github/workflows`, which the gateway's
token can't push; offer no Publish), `pr_title` (the title the agent wrote when
`pr_title_by_agent`, else the job's name), `pr_body` (exactly the description the
gateway will send) with `pr_body_sha256`, `mentions` (@handles in it), `closes` (issues
its closing keywords close on merge), `pr_footer` (what the gateway appends to a
description the user writes), and `pr_url` (an open pull request the job already
has; absent if none). The strings are raw, and the paths, title and description are the
agent's: show them as text, with invisible and direction-changing characters visible.
```json
{
   "type": "coding_publish_preview_response",
   "payload": {
      "job": 42, "nonce": "a1", "ok": true,
      "preview": {
         "head": "4f1c…", "repo": "owner/repo", "branch": "agent/42-fix", "base": "main",
         "commits": 2, "commits_more": false, "files": 3, "files_total": 3,
         "to_check": [], "history_only": [ { "path": "notes.txt", "to_check": false } ],
         "workflows": false, "pr_title": "Fix the parser", "pr_title_by_agent": true, "pr_body": "…", "pr_body_sha256": "9a0b…",
         "mentions": [], "closes": [ "#41" ]
      }
   }
}
```

#### `coding_publish_response`
`ok: true` with `message` (DAWN's line: what was pushed) and, when a pull request is
open, `pr_url`; or `ok: false` with `error` (why it wasn't published).
```json
{ "type": "coding_publish_response", "payload": { "job": 42, "nonce": "a2", "ok": true, "message": "Published coding job #42 …", "pr_url": "https://github.com/owner/repo/pull/12" } }
```

#### `coding_close_response`
`ok: true` with `closed` (bool) and `message`; when closing would delete unpublished
work, `closed` is false and `needs_confirm`: {`commits`, `files` (with uncommitted
changes), `branch`, `head` (`""` when there are no commits)} says what, to be sent back
with `confirm: true`. A refusal is `ok: false` with `error`, and no `closed`.
```json
{ "type": "coding_close_response", "payload": { "job": 42, "nonce": "a3", "ok": true, "closed": false, "message": "Closing coding job #42 (\"Fix the parser\") deletes its 3 unpublished commits and 2 files with uncommitted changes; nothing else has them.", "needs_confirm": { "commits": 3, "files": 2, "branch": "agent/42-fix", "head": "4f1c…" } } }
```

#### `coding_grants_response`
To every tab of the user. `grants` is absent (with `error`) when the gateway didn't
answer; after a revoke, `revoked`, `ok` and `message` say how it went. Each grant:
`kind` (`domain` or `tool`), `value` (as stored, control characters dropped, clipped:
show as text), `checked` (false: DAWN can't read it as a host or tool name; still
listed so it can be revoked), `repo` (absent: every repo).
```json
{
   "type": "coding_grants_response",
   "payload": {
      "revoked": 4, "ok": true, "message": "Removed standing approval 4 (...).",
      "grants": [ { "id": 3, "kind": "domain", "value": "pypi.org", "checked": true, "repo": "dawn" } ]
   }
}
```

#### `coding_reply_response`
```json
{
   "type": "coding_reply_response",
   "payload": { "job": 1977, "request_id": 12, "nonce": "c1x2y3-4", "ok": true, "message": "Allowed: coding job #1977 (...) may now reach the host `downloads.arduino.cc`, for this task only. The agent picks up again." }
}
```

#### Coding job events
A coding job's conversation carries `agent_step` (`gw`: `tool` with `tool`, `brief`;
`progress` with `text`), `agent_request` (`gw`: `request` with `request_id`,
`request_kind`, `line` (DAWN's checked wording, empty when it can't show it),
`value`, `tool`, `input`; `reply` with `request_id`, `decision`; `blocked`;
`input_required` with `request_ids`; after the job finished, `published` with
`pushed`, `pr_url`; `merged` with `pr_url`, `unpublished` (newer commits not
published); `closed` with `reason`: `user` or `merged`) and `agent_end` (`gw`:
`completed`, `failed`, `cancelled`, with `summary`, `branch`, `commits`, `diffstat`,
`uncommitted`, `to_check` with `to_check_count` (files to check before publishing, one per
line: the path, then a tab and the reason when there is one; `protected` and
`protected_count` from an older gateway), `error`, `cost_usd`; lists arrive joined by
newlines). All of it is the agent's text except `line`: render it as text
only (show `pr_url` only in GitHub's form, `https://github.com/<o>/<r>/pull/<n>`).

#### `job_action_response`
Result of a `job_action`. Sent on every outcome including refusal, so a client can
distinguish "refused" from "still working".
```json
{
   "type": "job_action_response",
   "payload": { "action": "resume", "conversation_id": 1009, "ok": true, "message": "Resuming." }
}
```

#### `list_jobs_response` — a page of terminal jobs
Reply to `list_jobs`. Keyset-paginated on `(created_at, id)` descending: echo
`next_before_created_at`/`next_before_id` back as the next request's cursor. The
id tiebreak matters — several jobs finishing within the same second is the common
case. `next_before_*` are omitted on an empty page.
```json
{
   "type": "list_jobs_response",
   "payload": {
      "jobs": [ /* … */ ],
      "has_more": true,
      "next_before_created_at": 1784949100,
      "next_before_id": 998
   }
}
```
`success` is `true` on a page. When the server can't read one it answers with
`success: false`, an `error` sentence and none of the other fields
(`{ "success": false, "error": "Couldn't read your finished jobs." }`): show the
error and treat the list as incomplete; the same request may be sent again.

#### `job_notification`
A background job finished: a silent completion toast to the owner's browser
sessions (no voice). Browsers only, and the delivery count is load-bearing — the
completion monitor records the job as "user told" when this reaches at least one
client, so it is never counted toward a satellite (which has no toast surface).
```json
{ "type": "job_notification", "payload": { "text": "Research on X is ready.", "conv_id": 1009, "running": 2 } }
```
- `conv_id`: the job's conversation id (open it to read the result)
- `running`: the user's remaining active-job count after this completion

### Session & State

#### `session`
Session token and auth state (sent on connect/reconnect).
```json
{
   "type": "session",
   "payload": {
      "token": "a1b2c3d4...",
      "authenticated": true,
      "username": "alice",
      "is_admin": false,
      "reconnected": true,
      "session_id": 42,
      "timezone": "America/Los_Angeles",
      "expires_at": 1795000000
   }
}
```
- `expires_at` — when the login ends (epoch seconds; logged in only). See How long a login lasts.
- `reconnected` — `true` when the connection adopted its **own** existing session via a
  valid reconnect token; `false` when it landed on a **fresh** session (server restart,
  idle-expiry, a session ended to make room for another client, or a reconnect that was
  evicted onto a new session). Clients use it to
  decide recovery: `reconnected:false` → issue `load_conversation` to rebuild LLM
  context; `true` → the lightweight `set_active_conversation` re-anchor suffices.
  Feature-detect it (older servers omit it → treat as "load").
- `session_id` — the adopted session's numeric id (correlation). Emitted **only on the
  authenticated frame** (omitted pre-auth).
- `timezone` — the IANA zone this user's times are in: their own My Settings zone, else the
  server's (`[localization] timezone`, else the system's). The server reads and speaks times in it; show times in
  it too (format with this `timeZone`, not the browser's), so the user sees what Friday says.
  Authenticated frame only; older servers omit it (use the browser's zone).

#### `login_expiry`
Sent to a connection when its user's action moved the login's expiry on (at most once a day), so a
client warning of the end sees the new time without waiting for a `pong`.
```json
{"type": "login_expiry", "payload": {"expires_at": 1795000000}}
```

#### `timezone_changed`
Sent after every My Settings save to each browser connection of that user, so it may repeat the
current zone. Same meaning as the `session` frame's `timezone`; when it differs, redraw what shows
a time or a day.
```json
{ "type": "timezone_changed", "payload": { "timezone": "America/Los_Angeles" } }
```

#### `session_superseded`
Another connection (a second tab, or the same browser reconnecting) has taken over this
session, so this connection is about to be closed. Emitted **immediately before** the
server closes the socket with WS close code **`4001` "superseded"**.
```json
{
   "type": "session_superseded",
   "payload": { "reason": "superseded" }
}
```
- Both signals are sent — the data frame **and** the 4001 close code — because a reverse
  proxy (dev http-proxy, nginx) **strips a custom WS close code** (a proxied browser sees
  a generic `1006`). The data frame proxies through intact, so it is the proxy-robust
  signal; the 4001 code covers the same-origin/prod path.
- Client contract: on this frame **or** `close.code === 4001`, do **not** auto-reconnect
  (that would ping-pong the two tabs) — show a takeover state and reclaim only on an
  explicit user gesture (which cleanly evicts the other connection). `reason` is optional.

#### `config`
WebUI configuration (sent after session).
```json
{
   "type": "config",
   "payload": {
      "audio_chunk_ms": 200,
      "music_enabled": true,
      "music_port": 3001,
      "aurora_enabled": false,
      "version": "2.0.0",
      "protocol": 1,
      "features": ["logout_closes_sockets"]
   }
}
```
- `music_enabled`: Whether the dedicated `dawn-music` audio server is running. When
  `false`, a client should not open the music socket.
- `music_port`: Port of the dedicated music-stream server (subprotocol `dawn-music`).
  Advertised so clients don't have to assume `main_port + 1`.
- `aurora_enabled`: the daemon serves the Aurora client at `/aurora/` (`[webui] aurora_path`
  is set). The WebUI shows its link to Aurora only then.
- `version`: The DAWN daemon version (`VERSION_NUMBER`, compile-time). Advertised so
  clients can detect the daemon version for compatibility/telemetry.
- `protocol`, `features`: the protocol version and feature flags (see
  [Protocol version and feature flags](#protocol-version-and-feature-flags)).

#### `server_features`
Sent once on connect, right after `config`: features compiled into this daemon.
```json
{"type": "server_features", "payload": {"home_assistant": true}}
```
- `home_assistant`: the Home Assistant tool is built in (not whether it's configured).

#### `config_changed`
An admin saved settings (`set_config` succeeded). Sent to every admin browser connection,
the saving one included. Empty payload: re-send `get_config` to see the new values.
```json
{"type": "config_changed", "payload": {}}
```

#### `turn_waiting`
Flag `turn_wait`.  The turn this session just sent waits for another turn running on the same
conversation, and later starts; sent to the waiting session's connection only.
```json
{"type": "turn_waiting",
 "payload": {"conversation_id": 123, "client_ref": "17", "waiting": true,
             "holder": "turn", "stop_after_ms": 5000}}
```
```json
{"type": "turn_waiting",
 "payload": {"conversation_id": 123, "client_ref": "17", "waiting": false}}
```
- `client_ref`: the waiting turn's, when it was sent with one (typed turns); voice turns have
  none.
- `holder`: what holds the conversation: `turn` (another tab or app of the user), `job` (a
  background job's turn) or `channel` (a messaging channel's turn).
- `stop_after_ms`: how long to show the wait before offering to stop the other reply
  (`stop_other_reply`).
- With `waiting: true` comes a `state` `thinking` whose `detail` says what it waits for (for
  clients without the flag). `waiting: false` is followed by the turn's normal frames.
- The other turn's rows arrive as `message_appended` meanwhile.
- A reconnect while the turn waits doesn't send `turn_waiting` again; the turn's frames follow
  when it starts.
- A turn whose connection is gone keeps waiting for up to the daemon's stale-turn time
  (`[jobs] max_runtime_sec`), then stops (`turn_unsent`).

#### `turn_unsent`
Flag `turn_wait`.  The turn stopped before its question was saved: while it waited (`cancel`,
its connection gone past the stale-turn time, or the daemon shutting down), or after it started
(a `cancel` meant for the turn before it can land here).  Followed by `state` `idle`.  A spoken
turn gets it without `client_ref`.  Not sent for a turn the client didn't send (a background job's result
re-engaging the conversation): that one is tried again later.
```json
{"type": "turn_unsent",
 "payload": {"conversation_id": 123, "client_ref": "17", "text_saved": false}}
```

#### `state`
State machine update.
```json
{
   "type": "state",
   "payload": {
      "state": "idle|thinking|speaking|error|listening|summarizing",
      "detail": "Fetching URL...",
      "tools": [{"name": "weather", "status": "running"}]
   }
}
```
- `detail`: Optional status message during long operations
- `tools`: Optional array of active tool calls (during parallel execution)

> **Always-on `recording` and `state`.** When the user says only the wake word (then speaks
> the command), the server speaks **no** greeting: the `always_on_state: "recording"` frame is
> the ready cue. A client that wants an acknowledgement plays a short non-speech chime, which
> the server's VAD won't take for speech if it bleeds into the microphone; a spoken
> acknowledgement played into the live microphone can keep the VAD from reaching end-of-speech.
> **Contract: during always-on `recording`, `always_on_state: "recording"` wins**: the client
> keeps its microphone open until recording clears (`recording` → `processing`, which precedes
> the reply's `state: "speaking"`), and doesn't mute on a `state` frame meanwhile.

#### `always_on_state`
Always-on mode's state, for this connection only.
```json
{"type": "always_on_state", "payload": {"state": "listening"}}
```
- `listening`: waiting for the wake word.
- `wake_check`: speech started; the utterance is being collected.
- `wake_pending`: the utterance ended and is being checked for the wake word (`listening`
  follows when it had none).
- `recording`: the wake word was heard on its own and the command is being recorded. This
  frame is the ready cue; the server speaks no greeting here (a client may play a short
  non-speech chime). A wake word with the command in the same utterance goes straight to
  `processing`.
- `processing`: the command is transcribed and answered. The client mutes its microphone
  here; `listening` follows when the answer's turn ends (`state` `idle`), or when nothing was
  heard or no turn started.
- `disabled`: always-on is off: on request, or turned off by the server after 60 seconds with
  no audio outside `processing` (4 minutes after an answer, so a long spoken reply can finish).
- Not every return to listening is announced: a wake-word check that stalls for 10 s, and a
  bare wake word followed by 6 s with no command, go back to listening without a frame, so a
  client may still show `wake_pending` or `recording`. Re-sync with `always_on_state_request`.
- While waiting in `processing`, a turn that runs too long is given up: the client gets
  `always_on_state` `listening` and an `error` frame `ALWAYS_ON_GAVE_UP`, severity `warning`,
  whose message says what happened (a spoken turn is stopped; a typed turn still running keeps
  running and the command runs after it).

#### `error`
Error or informational notification.
```json
{
   "type": "error",
   "payload": {
      "code": "LLM_TIMEOUT",
      "message": "Request timed out",
      "severity": "error",
      "recoverable": true
   }
}
```
- `severity`: `"info" | "warning" | "error"`. Not every `error` frame is a failure —
  DAWN also sends purely informational notices on this channel (e.g.
  `INFO_THINKING_DISABLED`, severity `"info"`). A client should route/style on
  `severity` rather than the code prefix. Absent field ⇒ treat as `"error"`.
- `recoverable`: Legacy field, currently always `true`. Prefer `severity`.
- A `text` turn refused for its images carries `IMAGE_UNAVAILABLE`, `IMAGE_LIMIT` or
  `IMAGE_ERROR`; one with neither text nor images, `EMPTY_MESSAGE`; one refused because too
  many are queued, `TURN_QUEUE_FULL` (see `text`). The turn did not run and nothing was saved;
  each refusal is one frame.
- `client_ref`: on an error raised for a `text` turn that carried one (flag `turn_refs`), the
  same string, unchanged; absent otherwise.
- `request_type`, `req`: on an error sent while a message from this connection is being
  handled (flag `request_refs`): the message's `type`, and its payload's `req` when it had
  one, each only when it is at most 64 bytes of valid UTF-8 with no control characters. An
  error raised later (a turn failing as it runs) or for no message has neither. An error is
  sent at once, so it can arrive before replies to earlier requests: match it by `req`, not
  by order.
- A message whose `type` this daemon doesn't handle (older daemon, or a feature not compiled
  in) is answered with `UNKNOWN_TYPE`, severity `"error"`, so a client can fail at once instead
  of waiting out its timeout. It carries `request_type` and `req` as above, and did before
  `request_refs`. Daemons before `UNKNOWN_TYPE` answer nothing; no feature flag marks it.

#### `force_logout`
This connection's login ended: a logout (from this or another tab), a login over it
in the same browser, a revoke (WebUI or `dawn-admin`), a password change, a deleted
user, or the login expiring. Sent **immediately before** the server closes the socket
with WS close code **`4002` "logged out"**, after any frame still going out; only a
client that doesn't read it within 3 seconds gets the close code alone. Frames the client sends in between
are ignored.
```json
{
   "type": "force_logout",
   "payload": {
      "reason": "Signed out"
   }
}
```
- Both signals, as with `session_superseded`: a reverse proxy strips the close code.
- Client contract: on this frame **or** `close.code === 4002`, drop the session token
  and per-user state, do **not** reconnect, and go to the login screen. The login is
  gone: a new connection is unauthenticated until the user logs in again.

---

### Transcript & Streaming

#### `transcript`
Complete message (non-streaming, or replayed history).
```json
{
   "type": "transcript",
   "payload": {
      "role": "user|assistant|satellite_response",
      "text": "Hello, how are you?",
      "replay": true,
      "server_saved": true
   }
}
```
- `replay`: true when sending conversation history on reconnect
- `server_saved`: present and `true` on a user-turn echo when the daemon already persisted
  the row (the normal case — the daemon owns user-turn persistence). The client uses it to
  skip its own save. The echo `text` is the clean user text; any `[IMAGE:<id>]` markers were
  persisted server-side, not sent here.
- `client_ref`: on the user-turn echo of a `text` frame that carried one (flag `turn_refs`),
  the same string, unchanged.

#### `stream_start`
Start of LLM token stream.
```json
{
   "type": "stream_start",
   "payload": {
      "stream_id": 1
   }
}
```

#### `stream_delta`
Incremental text chunk during LLM streaming.
```json
{
   "type": "stream_delta",
   "payload": {
      "stream_id": 1,
      "delta": "The weather"
   }
}
```

#### `stream_end`
End of LLM token stream.
```json
{
   "type": "stream_end",
   "payload": {
      "stream_id": 1,
      "reason": "complete|stopped|error|transient_error|tool_iteration",
      "will_persist": true
   }
}
```
`will_persist` is present only when the daemon will save the reply's row (it then arrives as
`message_appended`); the client doesn't save it.
`"stopped"` (flag `stopped_replies_saved`): the user's Stop ended the reply. With
`will_persist: true`, what had streamed is saved and arrives as `message_appended` with
`stopped: true`; without it, nothing of the reply was saved.

---

### Extended Thinking

#### `thinking_start`
Start of extended thinking/reasoning block.
```json
{
   "type": "thinking_start",
   "payload": {
      "stream_id": 1,
      "provider": "claude|openai|local"
   }
}
```

#### `thinking_delta`
Incremental thinking content.
```json
{
   "type": "thinking_delta",
   "payload": {
      "stream_id": 1,
      "delta": "Let me consider..."
   }
}
```

#### `thinking_end`
End of thinking block.
```json
{
   "type": "thinking_end",
   "payload": {
      "stream_id": 1,
      "has_content": true
   }
}
```

#### `reasoning_summary`
OpenAI o-series reasoning token summary (content not available).
```json
{
   "type": "reasoning_summary",
   "payload": {
      "stream_id": 1,
      "reasoning_tokens": 4096
   }
}
```

#### `tool_step`
Live cross-viewer tool step (server-authoritative fan-out; see
[SERVER_AUTHORITATIVE_PERSISTENCE_DESIGN.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/SERVER_AUTHORITATIVE_PERSISTENCE_DESIGN.md) §12h).
Fans one `tool_call` or `tool_result` to the user's OTHER browsers viewing the conversation **as
the turn runs**, so a bystander sees tool activity live. **Ephemeral** — no durable event is
written for this frame; the step is already persisted with the turn, so reload rebuilds it.
```json
{
   "type": "tool_step",
   "payload": {
      "conversation_id": 1204,
      "stream_id": 7,
      "kind": "tool_call",
      "payload": "{\"tool\":\"search\",\"tool_call_id\":\"toolu_015abc\",\"args\":{\"q\":\"...\"}}"
   }
}
```
- `kind`: `tool_call` or `tool_result`.
- `payload`: an **opaque, pre-redacted, size-capped JSON string** — forwarded verbatim and rendered
  as text, never re-parsed as trusted. Its inner object carries `tool` (name), an optional
  `tool_call_id`, and (`args` | `result`).
- `tool_call_id` (inside the inner `payload`): provider correlation id — the **same key**
  `load_conversation` emits on the `tool_calls` / `role:tool` rows, so a client pairs a live result
  to its call with one implementation across live and reload. Present on both `tool_call` and
  `tool_result`; omitted when the provider supplied none.
- `iter` (inside the inner `payload`): the 0-based tool-loop **iteration index** this step belongs
  to. Present on both `tool_call` and `tool_result` when known; omitted otherwise. A client seals its
  per-iteration group when `iter` changes — so grouping stays correct even for a tool-only iteration
  that streams no text (which emits no stream boundary). Absent → group by stream boundaries only.
- `error` (inside the inner `payload`, **`tool_result` only**): `true` iff the tool step was a
  **confirmed failure** — set at execute time from the tool result (structural failure OR a
  tool-self-reported hard failure), **never parsed from the result text**. **Omitted otherwise**, so
  a consumer reds the pill solely on the explicit `true` (a red-only signal: neutral = success or
  unknown; there is deliberately no "success" value). Back-compat: absent → neutral in both
  directions; either side may land first.
  - **Reload parity**: the failure is **persisted** on the `role:tool` message row
    (`messages.is_error`, schema v81) and surfaced on reload as `is_error: true` in the
    `load_conversation` message projection (omitted otherwise), so a reloaded conversation reds a
    failed pill just like the live signal. (The durable job-observe path also persists the whole
    `tool_step` payload to `conversation_events`, so a job conversation's attach/replay reds too.)
- `stream_id` is best-effort / informational.
- **Recipients**: the user's authenticated WEBUI browsers viewing the conversation, **excluding the
  origin by default** (the origin renders its own steps from its live stream). A connection that
  advertised the `tool_step_origin` capability (see `reconnect`) instead receives its **own** turn's
  `tool_step` frames and renders every step uniformly from this frame — no stream-derived path.

---

### Context & Metrics

#### `context`
Context window token usage update.
```json
{
   "type": "context",
   "payload": {
      "current": 1500,
      "max": 8192,
      "usage": 18.3,
      "threshold": 80.0
   }
}
```
- `usage`: Percentage of context used
- `threshold`: Compaction threshold percentage

#### `context_compacted`
Notification after automatic context compaction.
```json
{
   "type": "context_compacted",
   "payload": {
      "tokens_before": 7500,
      "tokens_after": 2000,
      "messages_summarized": 12,
      "level": 1,
      "conversation_id": 1703,
      "summary": "Summary of compacted messages..."
   }
}
```
`conversation_id` (absent when the conversation isn't saved yet) names the conversation that
was compacted; a client showing another conversation ignores the frame.

#### `metrics_update`
Real-time metrics for UI visualization (rings/gauges), and the turn's prompt-cache figures.
```json
{
   "type": "metrics_update",
   "payload": {
      "state": "idle",
      "ttft_ms": 450,
      "token_rate": 42.5,
      "context_percent": 35,
      "input_tokens": 48832,
      "cached_tokens": 48640,
      "cache_write_tokens": 192,
      "cache_saved_tokens": 43536,
      "cache_state": "warm",
      "conversation_id": 1703
   }
}
```
- The cache fields describe the session's **last completed LLM call**, and every frame carries
  them: a `"thinking"` frame repeats the previous call's figures, and one call's figures can
  arrive on several `"idle"` frames (each idle state change sends one). Read them from an
  `"idle"` frame, and to total across turns count one per turn (the first `"idle"` after its
  `stream_end`), never every frame.
- `input_tokens` is the whole prompt (the hit-rate denominator), `cached_tokens` what was read
  from the provider's cache (0 = a miss), `cache_write_tokens` only when non-zero,
  `cache_saved_tokens` the input tokens' worth saved (list prices; negative on a write-heavy call).
- `cache_state` (optional; absent before any call) says how the call should have cached:
  `warm` (read what the previous call left), `warm_miss` (should have and didn't: a
  regression worth reporting, or for OpenAI three warm calls in a row that read nothing),
  `first`, `ttl` (expired), `model`, `tools` (the tools or `tool_choice`), `system`, `thinking`
  (that part changed), `images` (the conversation's first image),
  `rewritten` (the history was compacted, a forgotten item withdrawn, or a turn taken back),
  `shared` (a local server another conversation used in between), or `untracked` (not judged:
  Gemini, other OpenRouter vendors, a local server other than llama.cpp).

#### `conversation_reset`
Notification that conversation context was reset (via tool).
```json
{"type": "conversation_reset"}
```

#### `context_citations`
Memory-citation signal (feature-gated on `[memory.decay] citation_enabled`). Emitted at turn
end when the model cited ≥1 injected memory, so a "Context for this turn" panel can
gold-highlight the rows the model actually used. Delivered only to the WEBUI session on
the matching active conversation.
```json
{
   "type": "context_citations",
   "conversation_id": 1163,
   "turn_id": 23474,
   "cited_item_ids": ["fact:8502", "summary:2496"]
}
```
- Fields are flat at the root (not under `payload`).
- `turn_id` is the triggering user message id; it pairs this frame to the per-turn
  focus block (the `context_injection` panel broadcast) by `(conversation_id, turn_id)`.
  `turn_id` is globally unique, so a consumer can match on it alone.
- `cited_item_ids` are the **validated** cited subset (hallucinated/out-of-range ordinals
  are already dropped server-side); each maps to a focus-block row's `item_id`. Only memory
  rows carry a citeable `item_id`.

---

### LLM State

#### `llm_state_update`
Proactive LLM configuration update (sent when session config changes).
```json
{
   "type": "llm_state_update",
   "payload": {
      "success": true,
      "type": "cloud",
      "provider": "claude",
      "model": "claude-sonnet-4-5",
      "follows_default": false,
      "openai_available": true,
      "claude_available": true,
      "gemini_available": false,
      "openrouter_available": false
   }
}
```
- `follows_default`: true when the conversation has no model of its own (it follows the
  dawn.toml default); a client offers "Use the default" only when it is false, sending
  `set_session_llm` `{"type": "reset"}` without `from_restore`, which saves the
  conversation as following the default
- `*_available`: whether that provider has an API key, for showing which can be picked
  (a provider without one is listed as "(no API key)"); never a choice made for the user

---

### Proactive & Ambient Broadcasts

Server-initiated pushes from the proactive-attention (SAGE), focus, and memory
subsystems. These are **WebUI-only UI primitives** (satellites do not render them)
and are routed to the owning user's browser sessions. A read-mostly dashboard
consumes them as ambient notices. This document is the contract for them; the Aurora
client's widget map (`aurora/docs/DAWN_UI_SIGNAL_MAP.md`) only says where it shows them.

#### `attention_alert`
The SAGE proactive-attention banner. Its own channel (not `scheduler_notification`)
so it shows an ATTENTION badge and never triggers the scheduler chime.
```json
{
   "type": "attention_alert",
   "payload": { "summary": "Package out for delivery, arriving today.", "level": "ambient" }
}
```
- `level`: `alert` (needs the user) or `ambient` (informational)

#### `watch_readings`
The watches' live values, about once a second, to each browser connection that sent
`watch_readings_subscribe` with `enabled: true`. Not sent while `[attention] enabled` is off, nor
when the user has no watches. Only the moving numbers: the rest of a watch comes from
`watch_list`. Every watch of the user is listed, disabled ones included.
```json
{
   "type": "watch_readings",
   "payload": {
      "readings": [
         { "id": 12, "has_current": true, "current": 612, "breaching": false },
         { "id": 15, "has_current": false, "breaching": false }
      ]
   }
}
```
- `current` is absent when `has_current` is false. An id not in the client's list means a watch
  was added elsewhere: ask `watch_list`.

#### `silent_observation`
A quieter rail-icon / peek primitive: DAWN noticed something worth surfacing
without a banner.
```json
{
   "type": "silent_observation",
   "payload": {
      "ts": 1784949199,
      "category": "calendar",
      "note": "Standup moved to 10:30.",
      "filter_match": true
   }
}
```
- `ts`: Unix seconds
- `filter_match`: whether it matched an active user watch/filter

A test frame can be asked for with
`{"type": "test_silent_observation", "payload": {"category": "calendar", "note": "test", "filter_match": false}}`
(a `payload` is required, and may be `{}`; its fields are optional: defaults `system`, `test event`, false), sent to the requesting user.
**Admin only** (`FORBIDDEN` otherwise), and only while `[debug] silent_observe_test_endpoint` is on;
with it off the request is dropped without a reply. No response frame.

#### `context_injection`
Diagnostic: what the focus system pulled into context for a turn (the "why did it
say that" surface). **Fields are at the root, not wrapped in `payload`.** Routed
only to the user's session(s) currently viewing `conversation_id`.
```json
{
   "type": "context_injection",
   "user_id": 3,
   "conversation_id": 1006,
   "turn_id": 42,
   "items": [
      {
         "source_id": "mem:918",
         "source_type": "internal",
         "text": "...",
         "score": 0.82,
         "score_breakdown": { "semantic": 0.5, "recency": 0.1, "importance": 0.2, "source": 0.02 },
         "applied_source_weight": 1.0,
         "item_timestamp": 1787500000,
         "provenance": { "conversation_id": 1000, "msg_id_start": 12, "msg_id_end": 14 }
      }
   ],
   "filter_rejections": [ { "source_id": "mem:77", "count": 2 } ]
}
```
- `source_type`: `internal`, `external`, or `user-content`
- `item_timestamp`: unix seconds the item was learned, saved or happens (the date the model
  sees after the item's source); omitted when it has none
- `provenance`: omitted entirely when unavailable (never a zero-stub)

#### `memory_extraction_notice`
DAWN stored or updated a memory during a turn.
```json
{ "type": "memory_extraction_notice", "payload": { "level": "info", "message": "Saved: prefers metric units." } }
```

#### `cache_alert`
A DAWN bug a person should report: the Anthropic API dropped earlier reasoning because a
request's prefix changed (`prefix_binding_mismatch`).  **Admin-only, browsers only**, once per
conversation per daemon run.  `message` is DAWN's own plain-language text (show it via
`textContent`); the WebUI keeps it as a toast until dismissed.
```json
{
   "type": "cache_alert",
   "payload": {
      "kind": "reasoning_dropped",
      "drops": 2,
      "model": "claude-opus-5-5",
      "message": "Earlier reasoning was dropped in a conversation: ..."
   }
}
```
The conversation isn't named (it may be another user's, or private); the daemon log's
`LLM binding` line names it.

#### `memory_batch`
A consolidation batch recorded, undone, kept from, or changed by a forget: the batch object, to every
browser tab of the user; one a forget removed: `{"batch_id", "removed": true}`. See Memory
Management, Consolidation.

#### `memory_proposals_changed`
The count of pending memory proposals changed (signal to refresh a proposals view).
```json
{ "type": "memory_proposals_changed", "payload": { "count": 3 } }
```

#### `ha_state_changed`
Real-time Home Assistant state delta. When `[home_assistant] realtime` is on (default),
DAWN subscribes to HA's own `/api/websocket` `state_changed` stream and pushes a
**coalesced delta of changed entities** (~200 ms window, so one scene-flip = one frame).
**Admin-only, browsers only.** Merge each element by `entity_id` into the entity model you
already build from `ha_entities_response`; a `{ "entity_id", "removed": true }` element is a
tombstone (drop it). Each non-removed element has the **same per-domain shape** as an
`ha_entities_response` entity (so it's a clean upsert). Feature-detect the frame `type` — a
client that ignores it stays correct via the entity poll backstop. **Bind these strings via
`textContent`/escaped templating, never `innerHTML`** (HA-controlled, unsolicited push).
```json
{
   "type": "ha_state_changed",
   "payload": {
      "entities": [
         { "entity_id": "light.office", "friendly_name": "Office Light", "domain": "light",
           "state": "on", "area": "Office", "attributes": { "brightness": 180 } },
         { "entity_id": "sensor.old", "removed": true }
      ]
   }
}
```
The Home Assistant requests (`ha_status`, `ha_test_connection`, `ha_list_entities`,
`ha_refresh_entities`, `ha_call_service`) and the `ha_entities_response` per-domain
`attributes` shape are documented under Home Assistant and Phone Audio (Admin Only); this
document is their contract. The Aurora client's widget map (`aurora/docs/DAWN_UI_SIGNAL_MAP.md
§9.4`) only says which widget sends and shows what.

---

### Phone

#### `phone_call_notification`
Inbound/outbound call state, for a transient call banner. Owner's browser sessions
only (carries caller PII). Also sent in reply to `phone_status`, which a client sends
after connecting to pick up a call already ringing or active; when no call is in
progress, `phone_status` gets no reply.
```json
{
   "type": "phone_call_notification",
   "payload": {
      "status": "ringing",
      "name": "Jane Doe",
      "number": "+15551234567",
      "call_id": 88,
      "elapsed_sec": 0,
      "photo": null
   }
}
```
- `status`: `ringing`, `active`, or `ended`
- `elapsed_sec`: call duration (for `active` / `ended`)
- `photo`: optional contact-photo object; omitted when none

#### `phone_action` (client → server)
Answer or reject a ringing call, or hang up the active one, from a call banner.
```json
{"type": "phone_action", "payload": {"action": "answer"}}
```
- `action`: `answer`, `reject` or `hangup`; anything else is an `error` frame
  `INVALID_PARAM`.
- Only the phone's owner (user id 1; the owner is not configurable yet), from a browser, may send it: anyone else gets
  `FORBIDDEN` "Not the modem owner".
- No reply: the result shows as `phone_call_notification` (`active` or `ended`). One action at
  a time: a `phone_action` sent while one is still running is dropped without a reply.

---

### Music

#### `music_state`
Current music playback state (sent on subscribe, control actions, track changes).
```json
{
   "type": "music_state",
   "payload": {
      "playing": true,
      "paused": false,
      "track": {
         "path": "/media/Music/song.flac",
         "title": "Bohemian Rhapsody",
         "artist": "Queen",
         "album": "A Night at the Opera",
         "duration_sec": 355
      },
      "position_sec": 42.5,
      "queue_length": 12,
      "queue_index": 3,
      "queue_rev": 57,
      "source_format": "flac",
      "source_rate": 44100,
      "quality": "high",
      "bitrate": 192000,
      "bitrate_mode": "vbr"
   }
}
```
- `queue_rev`: the queue's revision, changed by every edit. Fetch the list again
  when it changes, even when `queue_length` and `queue_index` don't (a replace can
  keep both)

#### `music_position`
Periodic playback position update.
```json
{
   "type": "music_position",
   "payload": {
      "position_sec": 43.5,
      "duration_sec": 355
   }
}
```

#### `music_error`
Music playback error.
```json
{
   "type": "music_error",
   "payload": {
      "code": "DECODE_ERROR",
      "message": "Failed to decode audio file"
   }
}
```

#### `music_search_response`
Results from music library search.

#### `music_library_response`
Music library browse results (artists, albums, tracks).

#### `music_queue_response`
Queue operation result.

---

### Scheduler Notifications

#### `scheduler_notification`
Sent when a scheduled event fires, is dismissed, or is snoozed, to the event owner's connections
(see Recipients below).
```json
{
   "type": "scheduler_notification",
   "payload": {
      "event_id": 42,
      "event_type": "alarm|timer|reminder|task",
      "status": "ringing|dismissed|snoozed|cancelled|fired",
      "name": "Morning Alarm",
      "message": "Morning Alarm",
      "fire_at": 1708300000,
      "conversation_id": 1234
   }
}
```
- `event_type`: `alarm`, `timer`, `reminder`, or `task`
- `status`: Current event status after the action
  - `ringing`: Event is actively firing (shows dismiss/snooze buttons)
  - `dismissed`: Event was dismissed by user
  - `snoozed`: Alarm was snoozed (will re-fire later)
  - `cancelled`: Event was cancelled
  - `fired`: Timer/reminder completed (auto-dismissed)
- `name`: Event name/label
- `message`: Display message (may include custom reminder text; briefings send a preview ~80 chars)
- `fire_at`: epoch **seconds** when the event fired. Present on every notification (including the
  missed-notification replay delivered on reconnect).
- `conversation_id` (optional): the conversation this notification is associated with, when it has
  one — e.g. a **briefing** that posted its output to a conversation. Clients use it for
  **click-to-open** (jump to that conversation). Absent (or `0`) for events with no conversation,
  such as a plain alarm or timer.
- Alarms pulse and support snooze; timers/reminders auto-dismiss after firing
- More fields: `tts_routed` (bool, live notifications): the event's speech is being played to
  a WebUI client, so skip the chime. `event_type` can also be `briefing`.
- **Missed notifications.** A ringing event of a user with no connection open is kept, and
  replayed to each new connection of that user (up to 32) as a `scheduler_notification` with
  `missed: true` and `missed_notif_id`. Replays repeat on every connect until a client sends
  `scheduler_action` `dismiss_missed` with that `missed_notif_id`, or until it is 24 hours old. A replay is history, never a
  live ring: don't offer snooze on it.
- Recipients: the event owner's connections, **satellites assigned to that user included**; an
  event with no user goes to every logged-in connection and every satellite.

#### `scheduler_events_changed`
Signal-only: the scheduled-event queue changed (event added, edited, or removed).
Empty payload; clients refetch the queue. Distinct from `scheduler_notification`,
which reports one event firing/dismiss/snooze.
```json
{ "type": "scheduler_events_changed" }
```

#### `calendar_events_changed`
Signal-only: a background CalDAV sync pulled changes for this user (empty payload; the
client refetches via `calendar_upcoming_events`). Emitted **only when something actually
changed** (ctag-gated), routed to the owning user's **browser** sessions (not satellites —
no calendar panel there). The sibling of `scheduler_events_changed` for calendar data;
carries no event content, so no PII on the wire.
```json
{ "type": "calendar_events_changed" }
```

#### `calendar_sync_done`
A sync the user asked for (`calendar_sync_account`, or adding an account) ended. Every
calendar sync runs on the server's sync thread, so `calendar_sync_account_response`
returns at once with `queued: true` and this follows when the sync ends, to the owner's
**browser** sessions. Older clients ignore it.
```json
{ "type": "calendar_sync_done", "payload": { "id": 3, "success": true } }
```

#### `conversation_renamed`
A conversation's title changed: the daemon titled it after its first exchange, or the user
renamed it from any client (`rename_conversation`; sent for a manual rename since 2026-11-17,
before that only for the daemon's own title).  Sent to every client of the owning user, the
renaming one included; that client may get it before its `rename_conversation_response`, so
don't rely on their order.
```json
{ "type": "conversation_renamed", "payload": { "conversation_id": 1234, "title": "Trip planning" } }
```
Update the title wherever the client shows it: the conversation list, an open conversation's
header, a memory batch's `conversation_title`.

#### `conversation_list_changed`
The user's conversation list changed — a conversation was **created** or its
last-activity was **bumped** — from any interface (a second browser tab, a messaging
channel, a voice/satellite turn, a background job, a scheduler briefing). Lets a browser
keep its conversation sidebar current without a page refresh. Fanned per-user to the
owning user's **browser** sessions only (a satellite/DAP client renders no sidebar).
Fired from the conversation-DB write paths (create + the per-message `updated_at` bump);
`role:"tool"` rows are suppressed server-side, so a multi-row tool turn emits one
`bumped`, not one per row. Carries a small payload so a client that maintains its list
in place can do a targeted update; the reference WebUI instead surfaces a consent pill
and re-fetches page-0 on click (it never re-orders the list silently). Unknown to older
clients — safely ignored.
```json
{
  "type": "conversation_list_changed",
  "payload": { "conversation_id": 1234, "reason": "created" }
}
```
`reason` is `"created"` (a new conversation appeared) or `"bumped"` (an existing
conversation's last-activity moved). Title/preview/timestamp are **not** included —
the client reads them from its normal `list_conversations` refetch, so nothing beyond
an id and a coarse reason is on the wire.

---

### Satellite Responses

#### `satellite_register_ack`
Registration confirmation for satellite.
```json
{
   "type": "satellite_register_ack",
   "payload": {
      "success": true,
      "session_id": 5,
      "reconnect_secret": "secret_for_reconnection",
      "session_token": "token_for_music_auth",
      "message": "Satellite registered successfully"
   }
}
```
- `reconnect_secret`: Client must save and provide on reconnection
- `session_token`: Used for music WebSocket authentication (a satellite's music
  socket needs no cookie: its session is bound by the device registration)

#### `satellite_pong`
Response to `satellite_ping`.
```json
{
   "type": "satellite_pong",
   "payload": {
      "server_time_ms": 1708300000000
   }
}
```
- `server_time_ms`: server wall-clock at reply time (epoch milliseconds).

Browser (WebUI) clients use the parallel `ping` → `pong` verb, which shares
this exact payload shape and additionally echoes a client-supplied `seq` (see
[`ping`](#ping) above). The two verbs differ only in the liveness gate: a
satellite `satellite_ping` is accepted on `is_satellite`, a browser `ping` is
authenticated via `conn_require_auth` (so a revoked/expired session gets an
`error` frame and **no** `pong`).

#### `volume_set`
Set the satellite's playback volume (the model's volume tool, on a satellite's own turn). The
satellite applies it and answers with `volume_state`.
```json
{"type": "volume_set", "payload": {"level": 65}}
```
`level`: 0–100.

Satellites also receive the same streaming messages as WebUI clients:
`state`, `error`, `transcript`, `stream_start`, `stream_delta`, `stream_end`.

---

### Request-Response Summary

| Request | Response |
|---------|----------|
| `get_config` | `get_config_response` |
| `stop_other_reply` | `stop_other_reply_response` |
| `email_list_accounts` | `email_list_accounts_response` |
| `email_add_account` | `email_add_account_response` |
| `email_update_account` | `email_update_account_response` |
| `email_remove_account` | `email_remove_account_response` |
| `email_set_read_only` | `email_set_read_only_response` |
| `email_set_enabled` | `email_set_enabled_response` |
| `email_test_connection` | `email_test_connection_response` |
| `email_list` | `email_list_response` |
| `email_search` | `email_search_response` |
| `email_read` | `email_read_response` |
| `email_set_flags` | `email_set_flags_response` |
| `email_unread_counts` | `email_unread_counts_response` |
| `email_archive` | `email_archive_response` |
| `email_trash` | `email_trash_response` |
| `email_undo` | `email_undo_response` |
| `set_config` | `set_config_response` |
| `set_secrets` | `set_secrets_response` |
| `get_audio_devices` | `get_audio_devices_response` |
| `list_models` | `list_models_response` |
| `list_interfaces` | `list_interfaces_response` |
| `list_llm_models` | `list_llm_models_response` |
| `restart` | `restart_response` |
| `ha_status` | `ha_status_response` |
| `ha_test_connection` | `ha_test_connection_response` |
| `ha_list_entities` | `ha_entities_response` |
| `ha_refresh_entities` | `ha_entities_response` |
| `ha_call_service` | `ha_call_service_response` |
| `get_phone_audio_config` | `phone_audio_config_response` |
| `set_session_llm` | `set_session_llm_response` |
| `get_system_prompt` | `system_prompt_response` |
| `get_tools_config` | `get_tools_config_response` |
| `set_tools_config` | `set_tools_config_response` |
| `get_metrics` | `get_metrics_response` |
| `always_on_enable` | `always_on_state` |
| `always_on_disable` | `always_on_state` |
| `always_on_state_request` | `always_on_state` |
| `list_users` | `list_users_response` |
| `create_user` | `create_user_response` |
| `delete_user` | `delete_user_response` |
| `change_password` | `change_password_response` |
| `unlock_user` | `unlock_user_response` |
| `list_satellites` | `list_satellites_response` |
| `update_satellite` | `update_satellite_response` |
| `delete_satellite` | `delete_satellite_response` |
| `get_satellite_registration_key` | `satellite_registration_key_response` |
| `ota_list` | `ota_list_response` |
| `ota_push` | `ota_push_response` |
| `ota_push_all` | `ota_push_all_response` |
| `ota_rollout_status` | `ota_rollout_status_response` |
| `ota_rollout_abort` | `ota_rollout_abort_response` |
| `get_my_settings` | `get_my_settings_response` |
| `set_my_settings` | `set_my_settings_response` |
| `list_my_sessions` | `list_my_sessions_response` |
| `revoke_session` | `revoke_session_response` |
| `list_channels` | `list_channels_response` |
| `create_link_code` | `create_link_code_response` |
| `unlink_channel` | `unlink_channel_response` |
| `reenable_channel` | `reenable_channel_response` |
| `rename_channel` | `rename_channel_response` |
| `verify_channel` | `verify_channel_response` |
| `resend_channel_code` | `resend_channel_code_response` |
| `set_channel_llm` | `set_channel_llm_response` |
| `list_conversations` | `list_conversations_response` |
| `new_conversation` | `new_conversation_response` |
| `load_conversation` | `load_conversation_response` |
| `set_active_conversation` | `set_active_conversation_response` |
| `delete_conversation` | `delete_conversation_response` |
| `rename_conversation` | `rename_conversation_response` |
| `set_private` | `set_private_response` |
| `set_pinned` | `set_pinned_response` |
| `conversation_learned_request` | `conversation_learned` |
| `forget_conversation_memories` | `forget_conversation_memories_response` |
| `reassign_conversation` | `reassign_conversation_response` |
| `export_conversation` | `export_conversation_response` |
| `search_conversations` | `search_conversations_response` |
| `save_message` | `save_message_response` |
| `update_context` | *(no response)* |
| `lock_conversation_llm` | `lock_conversation_llm_response` |
| `continue_conversation` | `continue_conversation_response` |
| `clear_session` | `clear_session_response` |
| `get_memory_stats` | `get_memory_stats_response` |
| `list_memory_facts` | `list_memory_facts_response` |
| `search_memory` | `search_memory_response` |
| `list_memory_entities` | `list_memory_entities_response` |
| `delete_memory_entity` | `delete_memory_entity_response` |
| `merge_memory_entities` | `merge_memory_entities_response` |
| `entity_aliases_request` | `entity_aliases_response` |
| `entity_merge_proposal_list_request` | `entity_merge_proposal_list_response` |
| `entity_link_request` | `entity_link_response` |
| `entity_unlink_request` | `entity_unlink_response` |
| `entity_proposal_resolve_request` | `entity_proposal_resolve_response` |
| `get_memory_fact_source` | `get_memory_fact_source_response` |
| `delete_memory_fact` | `delete_memory_fact_response` |
| `delete_memory_preference` | `delete_memory_preference_response` |
| `delete_memory_summary` | `delete_memory_summary_response` |
| `delete_all_memories` | `delete_all_memories_response` |
| `list_memory_superseded` | `list_memory_superseded_response` |
| `restore_memory_fact` | `restore_memory_fact_response` |
| `list_memory_batches` | `list_memory_batches_response` |
| `undo_memory_batch` | `undo_memory_batch_response` (when the worker ends) |
| `keep_memory_op` | `keep_memory_op_response` (when the worker ends) |
| `export_memories` | `export_memories_response` |
| `import_memories` | `import_memories_response` |
| `contacts_list` | `contacts_list_response` |
| `contacts_search` | `contacts_search_response` |
| `contacts_add` | `contacts_add_response` |
| `contacts_update` | `contacts_update_response` |
| `contacts_delete` | `contacts_delete_response` |
| `contacts_search_entities` | `contacts_search_entities_response` |
| `entity_set_photo` | `entity_set_photo_response` |
| `entity_ensure` | `entity_ensure_response` |
| `set_document_own` | `set_document_own_response` |
| `doc_library_list` | `doc_library_list_response` |
| `doc_library_get` | `doc_library_get_response` |
| `doc_library_delete` | `doc_library_delete_response` |
| `doc_library_index` | `doc_library_index_response` |
| `doc_library_toggle_global` | `doc_library_toggle_global_response` |
| `doc_library_note_save` | `doc_library_note_save_response` |
| `doc_library_note_update` | `doc_library_note_update_response` |
| `doc_library_doc_update` | `doc_library_doc_update_response` |
| `doc_library_version_list` | `doc_library_version_list_response` |
| `doc_library_deleted_list` | `doc_library_deleted_list_response` |
| `doc_library_version_restore` | `doc_library_version_restore_response` |
| `code_projects_list` | `code_projects_list_response` |
| `code_projects_import` | `code_projects_import_response` |
| `code_projects_link` | `code_projects_link_response` |
| `code_projects_refresh` | `code_projects_action_response` |
| `code_projects_rebuild` | `code_projects_action_response` |
| `code_projects_set_branch` | `code_projects_action_response` |
| `code_projects_delete` | `code_projects_action_response` |
| `music_subscribe` | `music_state` |
| `music_control` | `music_state` |
| `music_search` | `music_search_response` |
| `music_library` | `music_library_response` |
| `music_queue` | `music_queue_response` |
| `music_buffer` (music socket) | *(no response)* |
| `scheduler_action` | `scheduler_events_response` for `list`; otherwise none (broadcasts `scheduler_events_changed`, and `scheduler_notification` for a dismiss or snooze; `dismiss_missed` broadcasts only when its `event_id` is still ringing) |
| `watch_list` | `watch_list_response` |
| `watch_add` | `watch_add_response` |
| `watch_update` | `watch_update_response` |
| `watch_set_enabled` | `watch_set_enabled_response` |
| `watch_remove` | `watch_remove_response` |
| `watch_readings_subscribe` | `watch_readings_subscribe_response` |
| `calendar_list_accounts` | `calendar_list_accounts_response` |
| `calendar_add_account` | `calendar_add_account_response` |
| `calendar_edit_account` | `calendar_edit_account_response` |
| `calendar_remove_account` | `calendar_remove_account_response` |
| `calendar_test_account` | `calendar_test_account_response` |
| `calendar_sync_account` | `calendar_sync_account_response`, then `calendar_sync_done` |
| `calendar_list_calendars` | `calendar_list_calendars_response` |
| `calendar_toggle_calendar` | `calendar_toggle_calendar_response` |
| `calendar_toggle_read_only` | `calendar_toggle_read_only_response` |
| `calendar_set_enabled` | `calendar_set_enabled_response` |
| `calendar_list_my_calendars` | `calendar_list_my_calendars_response` |
| `calendar_upcoming_events` | `calendar_upcoming_events_response` |
| `oauth_get_auth_url` | `oauth_get_auth_url_response` |
| `oauth_exchange_code` | `oauth_exchange_code_response` |
| `oauth_disconnect` | `oauth_disconnect_response` |
| `oauth_check_scopes` | `oauth_check_scopes_response` |
| `phone_action` | *(no response; `phone_call_notification`)* |
| `test_silent_observation` | *(no response; `silent_observation`)* |
| `satellite_register` | `satellite_register_ack` |
| `satellite_ping` | `satellite_pong` |
| `volume_state` | *(no response)* |
| `ping` | `pong` |

---

### Response Payload Convention

All `*_response` messages follow a common pattern:
```json
{
   "type": "<request_type>_response",
   "payload": {
      "success": true,
      "error": "Error message if success is false",
      ...additional fields...
   }
}
```

---

## Source Files

| File | Purpose |
|------|---------|
| `src/webui/webui_server.c` | WebSocket server, streaming, state |
| `src/webui/webui_message_dispatch.c` | Main message dispatch |
| `src/webui/webui_broadcasts.c` | Push broadcasts: attention, silent-observation, context-injection, memory, job/conversation notices |
| `src/core/attention/attention_core.c` | Proactive-attention engine (drives `attention_alert`) |
| `src/webui/webui_scheduler.c` | Scheduler events + notifications |
| `src/webui/webui_phone.c` | Phone call notifications |
| `src/webui/webui_homeassistant.c` | Home Assistant status, test, entity list/refresh, service calls, state deltas |
| `src/webui/webui_phone_config.c` | Phone call-audio settings |
| `src/webui/webui_config.c` | Config get/set, audio devices, models |
| `src/webui/webui_satellite.c` | DAP2 satellite registration and queries |
| `src/webui/webui_admin_satellite.c` | Satellite administration: list, update, delete, registration key |
| `src/webui/webui_ota.c` | OTA releases, pushes and fleet rollouts |
| `src/webui/webui_music.c` | Music streaming |
| `src/webui/webui_music_handlers.c` | Music control, search, library, queue |
| `src/webui/webui_music_server.c` | The music socket: authentication, buffer reports |
| `src/webui/webui_history.c` | Conversation CRUD, search, context |
| `src/webui/webui_email.c` | Email account settings, test connection |
| `src/webui/webui_email_panel.c` | The mail panel's verbs (list, search, read, flags, counts) |
| `src/webui/webui_email_exec.c` | Email work off the socket thread, one task per account |
| `src/webui/webui_memory.c` | Memory facts, preferences, summaries, the entity graph, export and import |
| `src/webui/webui_contacts.c` | Contacts, entity photos, `entity_ensure` |
| `src/webui/webui_attention.c` | Proactive-attention watches and live readings |
| `src/webui/webui_calendar.c` | Calendar accounts, calendars, upcoming events |
| `src/webui/webui_oauth.c` | Google account linking (OAuth) |
| `src/webui/webui_doc_library.c` | Document library and notes |
| `src/webui/webui_code_projects.c` | Code projects |
| `src/webui/webui_messaging.c` | Messaging channels |
| `src/webui/webui_always_on.c` | Always-on voice (server-side VAD and wake word) |
| `src/webui/webui_memory_batches.c` | What memory learned per batch: list, undo, keep (the Learned tab, the marker) |
| `src/webui/webui_admin.c` | User management (CRUD, unlock) |
| `src/webui/webui_session.c` | Session list and revocation |
| `src/webui/webui_settings.c` | Personal user settings |
| `src/webui/webui_tools.c` | Tool enable/disable configuration |
| `src/webui/webui_audio.c` | Binary audio processing (Opus encode/decode) |
| `src/webui/webui_http.c` | HTTP routes, login, static files |
| `include/webui/webui_server.h` | Constants, binary types, public API |
