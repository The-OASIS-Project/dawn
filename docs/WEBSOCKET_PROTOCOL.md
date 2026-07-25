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
  `[webui] ssl_cert_path` is set, so use `wss://`.

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
Send a text message to the AI (with optional vision images).
```json
{
   "type": "text",
   "payload": {
      "text": "What is the weather?",
      "images": [
         {
            "data": "<base64-encoded image>",
            "mime_type": "image/jpeg"
         }
      ]
   }
}
```
- `images` is optional, max 5 images, max 4MB each
- Supported MIME types: `image/jpeg`, `image/png`, `image/gif`, `image/webp`
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
      "audio_codecs": ["opus", "pcm"]
   }
}
```
- If token is valid, session is restored with conversation history
- If token is invalid/expired, a new session is created
- `audio_codecs` is optional, used to detect Opus support

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

### Configuration (Admin Only)

#### `get_config`
Request the full daemon configuration.
```json
{"type": "get_config"}
```
Response: `get_config_response`

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
      "tool_mode": "native",
      "thinking_mode": "enabled",
      "reasoning_effort": "medium"
   }
}
```
- All fields are optional, only provided fields are changed
- `type`: `"local"`, `"cloud"`, or `"reset"` (revert to defaults)
- `tool_mode`: `"native"`, `"command_tags"`, or `"disabled"`
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
Response: `new_conversation_response`

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
Save a message to the current conversation in the database.
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
      "offset": 0
   }
}
```
Response: `list_memory_facts_response`

#### `list_memory_preferences`
List stored user preferences.
```json
{"type": "list_memory_preferences"}
```
Response: (uses `list_memory_facts_response` type with preference data)

#### `list_memory_summaries`
List conversation summaries.
```json
{"type": "list_memory_summaries"}
```
Response: (uses `list_memory_facts_response` type with summary data)

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
      "is_admin": false
   }
}
```

#### `config`
WebUI configuration (sent after session).
```json
{
   "type": "config",
   "payload": {
      "audio_chunk_ms": 200
   }
}
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

#### `error`
Error notification.
```json
{
   "type": "error",
   "payload": {
      "code": "LLM_TIMEOUT",
      "message": "Request timed out",
      "recoverable": true
   }
}
```

#### `force_logout`
Server-initiated logout (session revoked).
```json
{
   "type": "force_logout",
   "payload": {
      "reason": "Session revoked"
   }
}
```

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
      "replay": true
   }
}
```
- `replay`: true when sending conversation history on reconnect

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
      "summary": "Summary of compacted messages..."
   }
}
```

#### `metrics_update`
Real-time metrics for UI visualization (rings/gauges).
```json
{
   "type": "metrics_update",
   "payload": {
      "state": "thinking",
      "ttft_ms": 450,
      "token_rate": 42.5,
      "context_percent": 35
   }
}
```

#### `conversation_reset`
Notification that conversation context was reset (via tool).
```json
{"type": "conversation_reset"}
```

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
      "message": "Morning Alarm"
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
- `message`: Display message (may include custom reminder text)
- Alarms pulse and support snooze; timers/reminders auto-dismiss after firing
- Not sent to satellite connections (satellites don't have WebUI notification UI)

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
- `session_token`: Used for music WebSocket authentication

#### `satellite_pong`
Response to `satellite_ping`.
```json
{
   "type": "satellite_pong",
   "payload": {
      "timestamp": 1708300000
   }
}
```

Satellites also receive the same streaming messages as WebUI clients:
`state`, `error`, `transcript`, `stream_start`, `stream_delta`, `stream_end`.

---

### Request-Response Summary

| Request | Response |
|---------|----------|
| `get_config` | `get_config_response` |
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
| `delete_conversation` | `delete_conversation_response` |
| `rename_conversation` | `rename_conversation_response` |
| `set_private` | `set_private_response` |
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
| `delete_memory_fact` | `delete_memory_fact_response` |
| `delete_all_memories` | `delete_all_memories_response` |
| `music_subscribe` | `music_state` |
| `music_control` | `music_state` |
| `music_search` | `music_search_response` |
| `music_library` | `music_library_response` |
| `music_queue` | `music_queue_response` |
| `scheduler_action` | *(broadcast: `scheduler_notification`)* |
| `satellite_register` | `satellite_register_ack` |
| `satellite_ping` | `satellite_pong` |

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
| `src/webui/webui_config.c` | Config get/set, audio devices, models |
| `src/webui/webui_satellite.c` | DAP2 satellite registration and queries |
| `src/webui/webui_music.c` | Music streaming, search, library, queue |
| `src/webui/webui_history.c` | Conversation CRUD, search, context |
| `src/webui/webui_memory.c` | Memory facts, preferences, summaries |
| `src/webui/webui_admin.c` | User management (CRUD, unlock) |
| `src/webui/webui_session.c` | Session list and revocation |
| `src/webui/webui_settings.c` | Personal user settings |
| `src/webui/webui_tools.c` | Tool enable/disable configuration |
| `src/webui/webui_audio.c` | Binary audio processing (Opus encode/decode) |
| `src/webui/webui_http.c` | HTTP routes, login, static files |
| `include/webui/webui_server.h` | Constants, binary types, public API |
