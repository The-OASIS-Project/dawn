# DAWN WebSocket Protocol Reference

This document describes all WebSocket message types used between DAWN daemon,
WebUI browser clients, and DAP2 satellite devices. All connections use the
`dawn-1.0` WebSocket subprotocol on the same port (default 3000).

## Transport

- **Text messages**: JSON with `{"type": "...", "payload": {...}}`
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
- **Logging out**: `POST /api/auth/logout` (with `?app=<app>` for another front-end;
  same-origin: `Origin`, or a `Referer` whose `scheme://host[:port]` is exactly this
  server's or on `[webui] allowed_origins`) **with the socket open**. It ends that app's
  login only. The reply is immediate: `200 {"success":true}`, that app's cookie cleared
  (`Max-Age=0`), `Cache-Control: no-store`. Every connection on that
  login gets `force_logout` and is closed with **`4002`**, its music socket is closed
  by the server, and its sessions are destroyed, connected or not. A login over an
  existing one of the same app in the same browser ends the old one the same way.

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
| `email_client` | 2026-10-07 | The mail panel's verbs answer: `email_list`, `email_search`, `email_read`, `email_set_flags`, `email_unread_counts`, `email_archive`, `email_trash`, `email_undo`, and the server pushes `email_changed` (see Email). Advertised only while email is turned on (`[email] enabled`) and its service is up; a build without the WebUI or the email tool never advertises it. |
| `document_attachments` | 2026-10-02 | A `text` frame may carry its documents as `attachments` (`[{filename, size, content, blob_id?}]`): the daemon defuses each body and filename and builds the `[ATTACHED DOCUMENT: …]…[END DOCUMENT]` text itself, so a document can't end its own span. Without it, inline the documents into `text` as before. Every daemon with it also has `turn_refs`. |
| `turn_refs` | 2026-10-02 | A `text` frame may carry `client_ref`: the turn's own user `transcript` echo and every `error` raised for that turn (refused at receipt, refused or failed when it runs) carry it back unchanged, so a client knows which of its turns an error belongs to. Without it, refusals name no turn. |
| `visual_prompt_guard` | 2026-10-07 | A `text` frame may carry `from_visual: true` for a prompt a rendered visual sent; no confirm (email send or trash, call, text, document delete, door, research) counts in that turn. Send a visual's prompt only to a daemon with this flag, as its own turn; without it, drop the prompt and tell the user, never send it as an ordinary turn. |
| `image_only_turns` | 2026-10-01 | A `text` turn with no words (empty, absent or whitespace `text`) but at least one `image_ids` entry runs with just the images; one with neither is refused with `EMPTY_MESSAGE` instead of being dropped silently. Without it, require text with an image: older daemons drop a turn with no text without a reply. |
| `image_turns_by_id` | 2026-10-01 | A `text` turn takes images only through `image_ids`; `images[]` is ignored. A malformed, missing or foreign id, or too many, refuses the whole turn with `IMAGE_UNAVAILABLE` / `IMAGE_LIMIT` / `IMAGE_ERROR` and saves nothing (see `text`). Without it, send `images[]` with `image_ids` as before: older daemons send the model only `images[]`. |
| `app_logins` | 2026-09-30 | One login per app: `"app"` on login, `?app=` on every other request, a cookie per app (see Connection Lifecycle). Without it, all front-ends in a browser share one login. |
| `logout_closes_sockets` | 2026-09-30 | `POST /api/auth/logout` with the socket open is safe: the login's connections get `force_logout` and close with `4002`, the server closes the music socket, and the reply comes at once with the cookie cleared (see Connection Lifecycle, Logging out). Without it, close the socket before logging out. |

A client says who it is in its `init` / `reconnect` payload:
`"client": {"name": "aurora", "version": "1.4.0", "protocol": 1}`. DAWN logs it once per
connection (and warns on a protocol it doesn't speak); nothing is enforced on it.

## Connection Lifecycle

### WebUI Client Flow

1. HTTP login → cookie set → WebSocket connect
2. First message must be `reconnect` (with token) or any message (new session created)
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
                       "content": "<extracted text>", "blob_id": "blb_a1b2c3d4e5f6"}]
   }
}
```
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
  string of that shape (`null`, a number, empty, too long, or with other characters) refuses
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
  already queued for the session, `EMPTY_MESSAGE`, `ATTACHMENT_INVALID`, an `IMAGE_*` code, or
  `PROCESSING_ERROR` for anything else.
- Requires authentication

#### `cancel`
Cancel the current LLM operation for this session.
```json
{"type": "cancel"}
```

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
- If token is invalid/expired, a new session is created
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
  message (see Session Management). When that flag is set, the server slides this session's expiry
  forward on each authenticated heartbeat `ping` (up to a 30-day absolute cap from session creation).
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
Cancel or resume one background job. Ownership is checked against the connection's
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
  vocabulary as `set_llm_runtime`). This is the only place a fresh connection learns
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

#### `set_llm_runtime`
Switch LLM type/provider globally. **Admin only.** Affects all clients.
```json
{
   "type": "set_llm_runtime",
   "payload": {
      "type": "cloud",
      "provider": "claude"
   }
}
```
- `type`: `"local"` or `"cloud"`
- `provider`: `"openai"`, `"claude"`, or `"gemini"`
- Response: `set_llm_runtime_response`

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
- `type`: `"local"`, `"cloud"`, or `"reset"` (revert to defaults)
- `thinking_mode`: `"disabled"`, `"auto"`, or `"enabled"`
- `reasoning_effort`: `"low"`, `"medium"`, or `"high"`
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
      "user_id": 2
   }
}
```
Response: `delete_user_response`

#### `change_password`
Change a user's password.
```json
{
   "type": "change_password",
   "payload": {
      "user_id": 2,
      "new_password": "newpass123"
   }
}
```
Response: `change_password_response`

#### `unlock_user`
Unlock a locked-out user account.
```json
{
   "type": "unlock_user",
   "payload": {
      "user_id": 2
   }
}
```
Response: `unlock_user_response`

---

### Personal Settings (Authenticated)

#### `get_my_settings`
Get the current user's personal settings.
```json
{"type": "get_my_settings"}
```
Response: `get_my_settings_response`

#### `set_my_settings`
Update the current user's personal settings.
```json
{
   "type": "set_my_settings",
   "payload": {
      "display_name": "Alice",
      "timezone": "America/New_York"
   }
}
```
Response: `set_my_settings_response`

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
      "session_token": "abc123..."
   }
}
```
Response: `revoke_session_response`

#### `session_keepalive_enable`
Opt this browser's session into **keepalive** ("always on"). While the session actively heartbeats
(`ping`), the server slides its `expires_at` forward on renewal (within ~12h of expiry) instead of
letting it expire at the normal 24h — up to a **30-day absolute cap** measured from session creation.
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
Turn keepalive back off for this session; its expiry reverts to the normal fixed lifetime and is no
longer slid forward.
```json
{"type": "session_keepalive_disable"}
```
Requires an authenticated session. No response frame.

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
      "save_current": true
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
      "relations": 2
   }
}
```
- `memories` is what the Memory panel lists (facts, summaries and preferences); `outdated` are
  superseded facts it doesn't show; `relations` are links in the entity graph.
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
      "content": "The weather is sunny.",
      "thinking": "...",
      "tool_results": "[...]"
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
Lock a conversation to a specific LLM provider/model.
```json
{
   "type": "lock_conversation_llm",
   "payload": {
      "conversation_id": 42,
      "llm_type": "cloud",
      "llm_provider": "claude",
      "llm_model": "claude-sonnet-4-5"
   }
}
```
Response: `lock_conversation_llm_response`

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

Response: `list_memory_facts_response`

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
An empty `messages` array with `success: true` means the range held no
user/assistant messages (only system/tool). On failure, `success` is `false`
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
Response: `delete_memory_fact_response`

#### `delete_memory_preference`
Delete a specific user preference.
```json
{
   "type": "delete_memory_preference",
   "payload": {
      "preference_id": 3
   }
}
```
Response: (uses `delete_memory_fact_response` type)

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
Response: (uses `delete_memory_fact_response` type)

#### `delete_all_memories`
Delete all memories for the current user. Requires confirmation.
```json
{
   "type": "delete_all_memories",
   "payload": {
      "confirm": true
   }
}
```
Response: `delete_all_memories_response`

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

#### `music_subscribe`
Subscribe to music streaming for this connection.
```json
{
   "type": "music_subscribe",
   "payload": {
      "quality": "high",
      "audio_codecs": ["opus"]
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
      "action": "play|pause|resume|stop|next|previous|seek",
      "position_sec": 30.0
   }
}
```
- `action`: `play`, `pause`, `resume`, `stop`, `next`, `previous`, `seek`
- `position_sec`: only for `seek` action
- `play` behaviour depends on the payload and current state:
  - with `path` or `query` — plays that specific track / search hit (adds to top of queue)
  - bare (no `path`/`query`) while **paused** — resumes the pause
  - bare while **stopped** with a non-empty queue — starts the current `queue_index` track
    (index clamped into range); empty queue just re-echoes state
  - bare while **already playing** — re-echoes state (no-op)
- Response: `music_state` (updated state)

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
      "view": "artists|albums|tracks",
      "artist": "Queen",
      "album": "A Night at the Opera"
   }
}
```
Response: `music_library_response`

#### `music_queue`
Manage the playback queue.
```json
{
   "type": "music_queue",
   "payload": {
      "action": "add|clear|remove|play_index",
      "path": "/path/to/song.flac",
      "index": 0
   }
}
```
Response: `music_queue_response`

---

### Scheduler

#### `scheduler_action`
Dismiss, snooze, or cancel a scheduler event (alarm/timer/reminder).
```json
{
   "type": "scheduler_action",
   "payload": {
      "action": "dismiss|snooze|cancel",
      "event_id": 42,
      "snooze_minutes": 5
   }
}
```
- `action`: `dismiss` (stop ringing), `snooze` (reschedule, alarms only), `cancel` (delete pending)
- `event_id`: Database ID of the scheduled event
- `snooze_minutes`: Optional, defaults to configured snooze duration (default 5 min). Pass 0 for default.
- Requires authentication
- No direct response; server broadcasts updated `scheduler_notification` to all clients

---

### Calendar

Calendar account management (`calendar_list_accounts`, `calendar_add_account`, …) is
handled per-user in `webui_calendar.c`. The read-only *data* requests:

#### `calendar_list_my_calendars`
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

#### `calendar_upcoming_events`
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
         "wake_word": true
      },
      "reconnect_secret": "prev_secret_here"
   }
}
```
- `uuid`: 36-char UUID (8-4-4-4-12 hex format), required
- `name`: Display name (default: "Satellite")
- `location`: Room/location string
- `tier`: 1 (text-first, local ASR/TTS) or 2 (audio path, server ASR/TTS)
- `capabilities`: Tier 2 must NOT claim `local_asr` or `local_tts`
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
signal. The reply carries `{seq?, server_time_ms}` (same payload shape as
[`satellite_pong`](#satellite_pong)).

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

A user message sent from the WebUI is fanned out the same way (`"role": "user"`, its saved
text) to every browser of its user. The copy sent to the connection that sent it also carries
`client_ref` when the `text` frame had one (flag `turn_refs`), so that connection can match it
to its turn even if its `transcript` echo was dropped. Other connections' copies don't carry it.

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
`deliver_to` and `error` are **omitted when empty**. `status` ∈ `queued` |
`running` | `done` | `failed` | `interrupted` | `cancelled`; the first two are the
active states — treat anything else as terminal so an unrecognized future status
cannot pin a row in a client's active set forever.

#### `jobs_snapshot` — the complete active set
Reply to `jobs_request`; sent on (re)connect. Replaces the client's whole active
set. `truncated: true` means the server hit its row ceiling and any count derived
from this set is a **lower bound** — it should not happen under any valid config.
```json
{ "type": "jobs_snapshot", "payload": { "jobs": [ /* … */ ], "truncated": false } }
```

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
      "session_id": 42
   }
}
```
- `reconnected` — `true` when the connection adopted its **own** existing session via a
  valid reconnect token; `false` when it landed on a **fresh** session (server restart,
  idle-expiry, or a reconnect that was evicted onto a new session). Clients use it to
  decide recovery: `reconnected:false` → issue `load_conversation` to rebuild LLM
  context; `true` → the lightweight `set_active_conversation` re-anchor suffices.
  Feature-detect it (older servers omit it → treat as "load").
- `session_id` — the adopted session's numeric id (correlation). Emitted **only on the
  authenticated frame** (omitted pre-auth).

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
- `version`: The DAWN daemon version (`VERSION_NUMBER`, compile-time). Advertised so
  clients can detect the daemon version for compatibility/telemetry.
- `protocol`, `features`: the protocol version and feature flags (see
  [Protocol version and feature flags](#protocol-version-and-feature-flags)).

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

> **Precedence trap — `state: "speaking"` vs `always_on_state: "recording"`.**
> In the always-on bare-wake-word flow (user says only the wake word, then speaks a
> command), DAWN plays a short greeting ("Hello.") *while the always-on state machine
> is already in `recording`*. The greeting is normal TTS, so the client receives a
> top-level `state: "speaking"` **concurrent with** the `always_on_state: "recording"`
> frame. These read as contradictory. **Contract:
> during always-on `recording`, `always_on_state: "recording"` wins** — the client
> keeps its mic **open** and relies on client-side echo cancellation (AEC) to remove
> the greeting from its capture; it must **not** mute on `state: "speaking"` until
> recording clears (`recording` → `processing`, which precedes the *reply's*
> `state: "speaking"`). A client that mutes on `state: "speaking"` will gag its own
> command window. Corollary: the client's TTS playback must be **AEC-referenceable**
> (route through a media element the browser's echo canceller sees, not a bare Web
> Audio destination) or the greeting will bleed uncancelled into the live mic and
> keep the server's VAD from ever reaching end-of-speech.

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
- A message whose `type` this daemon doesn't handle (older daemon, or a feature not compiled
  in) is answered with `UNKNOWN_TYPE`, severity `"error"`, so a client can fail at once instead
  of waiting out its timeout. The frame carries `request_type` (the type sent) and `req` (the
  payload's `req`), each only when it is at most 64 bytes of valid UTF-8 with no control
  characters. Daemons before this answer nothing; no feature flag marks it. The error is sent
  at once, so it can arrive before replies to earlier requests: match it by `req`, not by order.

#### `force_logout`
This connection's login ended: a logout (from this or another tab), a login over it
in the same browser, a revoke (WebUI or `dawn-admin`), a password change, a deleted
user, or the login expiring. Sent **immediately before** the server closes the socket
with WS close code **`4002` "logged out"** (skipped only if a large frame is still
being sent: then the close code alone says it). Frames the client sends in between
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
      "reason": "complete|cancelled|error"
   }
}
```

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
Memory-citation signal (feature-gated on `[memory] citation_enabled`). Emitted at turn
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
      "openai_available": true,
      "claude_available": true,
      "gemini_available": false
   }
}
```

---

### Proactive & Ambient Broadcasts

Server-initiated pushes from the proactive-attention (SAGE), focus, and memory
subsystems. These are **WebUI-only UI primitives** (satellites do not render them)
and are routed to the owning user's browser sessions. A read-mostly dashboard
consumes them as ambient notices; see the consumer-facing map in
`dawn-nextgen/docs/DAWN_UI_SIGNAL_MAP.md`.

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
The HA request verbs (`ha_list_entities` / `ha_refresh_entities` / `ha_call_service` etc.)
and the `ha_entities_response` per-domain `attributes` shape are documented consumer-side in
`dawn-nextgen/docs/DAWN_UI_SIGNAL_MAP.md §9.4`.

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
      "source_format": "flac",
      "source_rate": 44100,
      "quality": "high",
      "bitrate": 192000,
      "bitrate_mode": "vbr"
   }
}
```

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
Broadcast to all authenticated WebUI clients when a scheduled event fires, is dismissed, or is snoozed.
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
- Not sent to satellite connections (satellites don't have WebUI notification UI)

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

Satellites also receive the same streaming messages as WebUI clients:
`state`, `error`, `transcript`, `stream_start`, `stream_delta`, `stream_end`.

---

### Request-Response Summary

| Request | Response |
|---------|----------|
| `get_config` | `get_config_response` |
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
| `set_llm_runtime` | `set_llm_runtime_response` |
| `set_session_llm` | `set_session_llm_response` |
| `get_system_prompt` | `system_prompt_response` |
| `get_tools_config` | `get_tools_config_response` |
| `set_tools_config` | `set_tools_config_response` |
| `get_metrics` | `get_metrics_response` |
| `list_users` | `list_users_response` |
| `create_user` | `create_user_response` |
| `delete_user` | `delete_user_response` |
| `change_password` | `change_password_response` |
| `unlock_user` | `unlock_user_response` |
| `get_my_settings` | `get_my_settings_response` |
| `set_my_settings` | `set_my_settings_response` |
| `list_my_sessions` | `list_my_sessions_response` |
| `revoke_session` | `revoke_session_response` |
| `list_conversations` | `list_conversations_response` |
| `new_conversation` | `new_conversation_response` |
| `load_conversation` | `load_conversation_response` |
| `set_active_conversation` | `set_active_conversation_response` |
| `delete_conversation` | `delete_conversation_response` |
| `rename_conversation` | `rename_conversation_response` |
| `set_private` | `set_private_response` |
| `conversation_learned_request` | `conversation_learned` |
| `forget_conversation_memories` | `forget_conversation_memories_response` |
| `reassign_conversation` | `reassign_conversation_response` |
| `search_conversations` | `search_conversations_response` |
| `save_message` | `save_message_response` |
| `update_context` | *(no response)* |
| `lock_conversation_llm` | `lock_conversation_llm_response` |
| `continue_conversation` | `continue_conversation_response` |
| `clear_session` | `clear_session_response` |
| `get_memory_stats` | `get_memory_stats_response` |
| `list_memory_facts` | `list_memory_facts_response` |
| `search_memory` | `search_memory_response` |
| `get_memory_fact_source` | `get_memory_fact_source_response` |
| `delete_memory_fact` | `delete_memory_fact_response` |
| `delete_all_memories` | `delete_all_memories_response` |
| `music_subscribe` | `music_state` |
| `music_control` | `music_state` |
| `music_search` | `music_search_response` |
| `music_library` | `music_library_response` |
| `music_queue` | `music_queue_response` |
| `scheduler_action` | *(broadcast: `scheduler_notification`)* |
| `calendar_list_my_calendars` | `calendar_list_my_calendars_response` |
| `calendar_upcoming_events` | `calendar_upcoming_events_response` |
| `satellite_register` | `satellite_register_ack` |
| `satellite_ping` | `satellite_pong` |
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
| `src/webui/webui_server.c` | Main message dispatch, streaming, state |
| `src/webui/webui_broadcasts.c` | Push broadcasts: attention, silent-observation, context-injection, memory, job/conversation notices |
| `src/core/attention/attention_core.c` | Proactive-attention engine (drives `attention_alert`) |
| `src/webui/webui_scheduler.c` | Scheduler events + notifications |
| `src/webui/webui_phone.c` | Phone call notifications |
| `src/webui/webui_homeassistant.c` | Home Assistant entity list/state |
| `src/webui/webui_config.c` | Config get/set, audio devices, models |
| `src/webui/webui_satellite.c` | DAP2 satellite registration and queries |
| `src/webui/webui_music.c` | Music streaming, search, library, queue |
| `src/webui/webui_history.c` | Conversation CRUD, search, context |
| `src/webui/webui_email.c` | Email account settings, test connection |
| `src/webui/webui_email_panel.c` | The mail panel's verbs (list, search, read, flags, counts) |
| `src/webui/webui_email_exec.c` | Email work off the socket thread, one task per account |
| `src/webui/webui_memory.c` | Memory facts, preferences, summaries |
| `src/webui/webui_admin.c` | User management (CRUD, unlock) |
| `src/webui/webui_session.c` | Session list and revocation |
| `src/webui/webui_settings.c` | Personal user settings |
| `src/webui/webui_tools.c` | Tool enable/disable configuration |
| `src/webui/webui_audio.c` | Binary audio processing (Opus encode/decode) |
| `src/webui/webui_http.c` | HTTP routes, login, static files |
| `include/webui/webui_server.h` | Constants, binary types, public API |
