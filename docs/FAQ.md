# DAWN FAQ

## Administration

### How do I view the security audit log?

Use `dawn-admin log show` from the command line. The daemon must be running.

```bash
# Show last 50 entries (default)
dawn-admin log show

# Show last 100 entries
dawn-admin log show --last 100

# Filter by event type
dawn-admin log show --type LOGIN_FAILED
dawn-admin log show --type RATE_LIMITED
dawn-admin log show --type PASSWORD_CHANGED

# Filter by username
dawn-admin log show --user jon

# Combine filters
dawn-admin log show --last 200 --type LOGIN_FAILED --user guest
```

**Event types logged**: `LOGIN_SUCCESS`, `LOGIN_FAILED`, `ADMIN_AUTH_FAILED`, `USER_CREATED`, `USER_DELETED`, `PASSWORD_CHANGED`, `USER_UNLOCKED`, `SESSION_REVOKED`, `USER_SESSIONS_REVOKED`, `SETTINGS_UPDATED`, `PERMISSION_DENIED`, `RATE_LIMITED`, `CSRF_FAILED`, `IP_UNBLOCKED`, `DB_COMPACT`, `DB_BACKUP`, `CONVERSATION_DELETED`

### How do I manage users?

```bash
# List all users
dawn-admin user list

# Create an admin user (requires DAWN_SETUP_TOKEN)
dawn-admin user create admin --admin

# Change a user's password
dawn-admin user passwd username

# Delete a user
dawn-admin user delete username

# Unlock a locked account (after too many failed logins)
dawn-admin user unlock username
```

### How do I manage sessions?

```bash
# List active sessions
dawn-admin session list

# Revoke a specific session by token prefix
dawn-admin session revoke a1b2c3d4

# Revoke all sessions for a user
dawn-admin session revoke --user guest
```

### How do I back up the database?

```bash
# Show database statistics
dawn-admin db status

# Back up to a file
dawn-admin db backup /path/to/backup.db

# Compact the database (reclaim space)
dawn-admin db compact
```

### How do I unblock a rate-limited IP?

```bash
# List IPs with failed login attempts
dawn-admin ip list

# Unblock a specific IP
dawn-admin ip unblock 192.168.1.100

# Unblock all IPs
dawn-admin ip unblock --all
```

## Scheduler

### What types of scheduled events are supported?

- **Timer**: Duration-based ("set a 10 minute timer")
- **Alarm**: Time-based, supports recurrence ("set an alarm for 7am every weekday")
- **Reminder**: Message-based ("remind me to call Mom at 3pm")
- **Task**: Fire-and-forget tool execution ("turn off lights at midnight")
- **Briefing**: Tool execution + LLM summary + persistent conversation ("weather briefing every morning at 7am")

### Which tools can be scheduled?

Tools with `TOOL_CAP_SCHEDULABLE`: `weather`, `calendar`, `search`, `email`, `url_fetch`, `homeassistant`, `volume`, `music`

### Where does TTS play when a scheduled event fires?

TTS routes based on where the event was created:
- **Created from WebUI**: TTS plays in the browser
- **Created from satellite**: TTS plays on the originating satellite (falls back to other user sessions if disconnected)
- **Created from local mic**: TTS plays on the daemon speaker

All sources also send a WebUI notification banner.

### Can I schedule emails?

Yes. Email has `TOOL_CAP_DANGEROUS` (requires LLM confirmation during conversation) but is also `TOOL_CAP_SCHEDULABLE`. Scheduling the email IS the confirmation — you explicitly asked for it.

```
"Send Bob the meeting details at 8am tomorrow"
"Check my email every morning at 7am and brief me"
```

## Music

### How do I manage the music library?

```bash
# Show music database statistics
dawn-admin music stats

# Search by artist, title, or album
dawn-admin music search "pink floyd"

# List tracks
dawn-admin music list --limit 50

# Trigger a library rescan
dawn-admin music rescan
```

## Memory

### How do I re-extract memory from scratch after extraction-prompt or model improvements?

When the extraction prompt, model, or post-processing logic changes meaningfully, derived state (facts, entities, relations, summaries, preferences, embeddings) can become stale relative to what the current pipeline would produce from the same conversations.  Conversations + messages are the source of truth; everything derived can be rebuilt from them.

```bash
# 1. Dry-run — reports conversation count, message count, estimated cost, provider/model
dawn-admin memory reextract --user jon

# 2. Review the estimate.  If reasonable, run for real:
dawn-admin memory reextract --user jon --confirm

# 3. Poll progress (the daemon processes conversations in the background)
dawn-admin memory reextract-status --user jon
```

Optional flags:

- `--keep-summaries` — preserve `memory_summaries` rows; useful when only fact extraction changed
- `--backup-path <path>` — write a SQLite Online Backup of `auth.db` before reset (recommended for first runs)
- `--max-cost-usd <X>` — abort `--confirm` if the cost estimate exceeds this ceiling

The reset is transactional (`BEGIN IMMEDIATE`) — if anything fails partway, derived state is left untouched.  The re-extraction itself runs through the existing recovery worker, which respects `[memory.recovery]` rate limits.

## Troubleshooting

### The daemon isn't responding to dawn-admin commands

The daemon must be running for `dawn-admin` to connect. It communicates via a Unix domain socket. Check:

```bash
# Is the daemon running?
ps aux | grep dawn

# Check the socket exists
ls -la /tmp/dawn-admin.sock
```

### A scheduled briefing said "tool disabled"

The tool needs both `TOOL_CAP_SCHEDULABLE` in its code AND to be listed in `local_enabled`/`remote_enabled` in `dawn.toml`. Check your config.

### Claude API returns "thinking.signature: Field required"

This was fixed — the thinking signature buffer is now dynamic. Update to the latest build.

## Architecture

### Why doesn't DAWN support MCP (Model Context Protocol)?

DAWN does not implement MCP. While MCP has become an industry standard for connecting LLMs to external tools, DAWN's architecture serves different goals:

- **Native C/C++**: DAWN is implemented entirely in C/C++ for reliability, deterministic timing, and single-binary deployment. MCP's ecosystem is built around TypeScript and Python. No C implementation exists, and embedding a managed runtime would negate the architectural benefits.
- **Voice-first responsiveness**: MCP's process-per-server model with JSON-RPC introduces latency. Voice assistants require sub-second response times. DAWN's direct function calls and shared-memory architecture eliminate IPC overhead entirely.
- **Integrated tool system**: DAWN's native tool execution provides parallel thread-pool execution, automatic schema generation for multiple LLM providers (OpenAI, Claude, Gemini, llama.cpp), session-scoped filtering, and built-in iterative tool loops. MCP defines a transport protocol — these capabilities remain the host's responsibility.
- **Self-contained design**: DAWN is a complete voice assistant, not a plugin framework. All tools are compiled into the binary and audited at the source level. There is no extension marketplace, no third-party plugins, no arbitrary code execution.

See `ARCHITECTURE.md` for the full discussion.

### Why doesn't DAWN use Wyoming or ESPHome protocols?

DAWN uses its own DAP2 WebSocket protocol instead of Home Assistant's Wyoming or ESPHome Voice Assistant protocols. The reasons are architectural, not "not invented here" syndrome:

**Different satellite philosophy**: DAWN Tier 1 satellites (Raspberry Pi) run the full voice pipeline locally — wake word, VAD, ASR, and TTS — and send only text (~100 bytes per interaction vs ~420KB for audio-streaming protocols). This is fundamentally incompatible with Wyoming and ESPHome, which treat all satellites as audio endpoints.

**LLM-native, not intent-based**: Wyoming and ESPHome route through Home Assistant's intent recognition system (rule-based NLU). DAWN routes through LLM providers with tool calling, conversation history, persistent memory, and extended thinking. These are different processing models.

**Streaming architecture**: DAWN streams LLM responses at sentence boundaries for real-time TTS — the user hears the first sentence while the LLM generates the fifth. Both Wyoming and ESPHome use sequential pipelines (ASR completes → process → TTS generates full response → stream back).

**No Home Assistant dependency**: DAWN is standalone. Adding HA as a dependency would introduce a large Python application into a pure C/C++ embedded system. ESPHome devices are useless without HA.

**Single-port simplicity**: DAWN serves WebUI, Tier 1 satellites, Tier 2 satellites, and music from one WebSocket port. Wyoming requires a separate TCP port per service.

| Aspect | DAWN DAP2 | Wyoming | ESPHome VA |
|--------|-----------|---------|------------|
| Satellite intelligence | Tier 1: full local pipeline; Tier 2: audio endpoint | Audio endpoint only | Audio endpoint + wake word |
| Processing model | LLM-native with tools + memory | Intent-based NLU | Intent-based (LLM bolted on) |
| TTS streaming | Sentence-level progressive | Sequential | Sequential |
| Music/media | Opus streaming + FFT visualization | Not supported | Media player (FLAC/MP3/WAV) |
| Transport | WebSocket (single port) | TCP (port per service) | TCP + protobuf |
| Security | TLS + session auth | None | Noise Protocol (optional) |
| Language | C/C++ throughout | Python | C++ (device) + Python (HA) |
| Server dependency | Standalone | Home Assistant | Home Assistant |

**Ideas adopted from them**: mDNS discovery (planned), audio ducking (planned), timer lifecycle (shipped as scheduler system).
