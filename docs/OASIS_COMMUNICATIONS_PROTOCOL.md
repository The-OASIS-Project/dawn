# OASIS Communications Protocol (OCP) v1.5

A standardized messaging protocol for inter-component communication within The OASIS Project.

## Overview

The OASIS Communications Protocol defines a consistent message format for communication between OASIS components (Dawn, Echo, Mirage, Stat, and future modules). The protocol covers request/response correlation, unsolicited event notification, component status/keepalive, capability discovery, and flexible data transport.

## Design Principles

1. **Correlation**: Every request can be correlated with its response via `request_id`
2. **Backward Compatible**: Components should handle messages with or without OCP fields
3. **Simple**: Minimal required fields, optional extensions
4. **Transport Agnostic**: Works over MQTT, WebSockets, HTTP, or any JSON transport
5. **Data Flexible**: Supports both references (file paths) and inline data

## Message Format

### Request Message

```json
{
  "device": "viewing",
  "action": "look",
  "value": "describe what you see",
  "request_id": "dawn_worker0_42"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `device` | Yes | Target device/capability (e.g., "viewing", "weather", "homeassistant") |
| `action` | Yes | Action to perform (e.g., "look", "get", "on", "off") |
| `value` | No | Action parameter (string, meaning depends on device/action) |
| `request_id` | No* | Unique identifier for correlating response. *Required if response needed |
| `session_id` | No | Client session ID for per-session context |
| `data` | No | Binary/structured data payload (see Data Transport section) |
| `timestamp` | No | Unix milliseconds when message was created (for debugging/ordering) |

### Response Message

```json
{
  "device": "viewing",
  "action": "completed",
  "value": "/path/to/snapshot.jpg",
  "request_id": "dawn_worker0_42",
  "status": "success"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `device` | Yes | Echoed from request |
| `action` | Yes | Result action (typically "completed" or original action) |
| `value` | No | Result data (simple string value or file path) |
| `request_id` | Yes* | Echoed from request. *Required if present in request |
| `status` | No | "success" or "error" (assumed "success" if omitted) |
| `error` | No | Error details if status is "error" |
| `data` | No | Binary/structured data payload (see Data Transport section) |
| `timestamp` | No | Unix milliseconds when response was created |
| `checksum` | No | SHA256 hash when `value` contains a file path |

### Error Response

```json
{
  "device": "viewing",
  "action": "completed",
  "request_id": "dawn_worker0_42",
  "status": "error",
  "error": {
    "code": "CAMERA_UNAVAILABLE",
    "message": "Camera device not found or busy"
  }
}
```

### Event Message

Events are unsolicited notifications — something happened, and the publishing component is informing subscribers. They differ from requests in that they are informational and do not expect a response.

```json
{
  "device": "echo",
  "event": "sms_received",
  "msg_type": "event",
  "timestamp": 1713100000000,
  "sender": "+15551234567",
  "body": "Hello"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `device` | Yes | Component that generated the event |
| `event` | Yes | What happened (e.g., "sms_received", "call_connected", "modem_lost") |
| `msg_type` | See below | `"event"` — required on shared topics, recommended on dedicated event topics |
| `timestamp` | Recommended | Unix milliseconds when the event occurred |
| Additional fields | No | Event-specific payload (top-level fields, or `data` block for binary) |

**Key distinction**: `action` is a verb requesting behavior ("look", "dial", "play"). `event` is a noun reporting something that happened ("sms_received", "call_ended", "modem_lost"). Components MUST NOT reuse `action` for event names or `event` for commands.

### Event vs Request

| Aspect | Request | Event |
|--------|---------|-------|
| Field name | `action` | `event` |
| Direction | Sender → target | Publisher → subscribers |
| Response expected | Yes (if `request_id` present) | No |
| Semantic | Imperative ("look", "dial") | Indicative ("sms_received", "call_ended") |
| `msg_type` | `"request"` (implicit) | `"event"` |

Events SHOULD be published on dedicated event topics (e.g., `echo/events`) rather than shared command topics. When published on a shared topic (e.g., `hud`), `msg_type: "event"` is REQUIRED to distinguish from commands.

## Data Transport

For transferring binary data (images, audio, files) or structured data, OCP provides two approaches:

### Option 1: Reference (File Path)

Use `value` field with a file path. Suitable when components share filesystem access.

```json
{
  "device": "viewing",
  "action": "completed",
  "value": "/home/jetson/recordings/snapshot-20231220_143052.jpg",
  "request_id": "dawn_worker0_1",
  "status": "success",
  "checksum": "a1b2c3d4e5f6..."
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `value` | Yes | File path to the data |
| `checksum` | No | SHA256 hash of file contents (hex-encoded, lowercase) |

**Pros**: Small message size, efficient for large files
**Cons**: Requires shared filesystem, tight coupling

### Option 2: Inline Data

Use `data` field with encoding specification. Suitable for decoupled or remote components.

```json
{
  "device": "viewing",
  "action": "completed",
  "request_id": "dawn_worker0_1",
  "status": "success",
  "data": {
    "type": "image/jpeg",
    "encoding": "base64",
    "content": "/9j/4AAQSkZJRgABAQEASABIAAD..."
  }
}
```

| Data Field | Required | Description |
|------------|----------|-------------|
| `type` | Yes | MIME type (e.g., "image/jpeg", "audio/wav", "application/json") |
| `encoding` | Yes | Encoding used for content (see Supported Encodings below) |
| `content` | Yes | The encoded data |
| `size` | No | Original size in bytes before encoding (for validation) |
| `checksum` | No | SHA256 hash of original data (hex-encoded, lowercase) |

### Supported Encodings

| Encoding | Use Case | Description |
|----------|----------|-------------|
| `base64` | Binary data | Standard base64 encoding (RFC 4648) |
| `utf8` | Text data | UTF-8 encoded text string |
| `none` | JSON objects | Content is a raw JSON object/array (no encoding) |

### Data Type Examples

**Image (base64)**:
```json
{
  "data": {
    "type": "image/jpeg",
    "encoding": "base64",
    "content": "/9j/4AAQSkZJRgABAQEASABIAAD...",
    "size": 145017
  }
}
```

**Structured JSON**:
```json
{
  "data": {
    "type": "application/json",
    "encoding": "none",
    "content": {
      "temperature": 72,
      "humidity": 45,
      "conditions": "partly cloudy"
    }
  }
}
```

**Text**:
```json
{
  "data": {
    "type": "text/plain",
    "encoding": "utf8",
    "content": "The current temperature is 72 degrees."
  }
}
```

### Choosing Reference vs Inline

| Use Case | Recommended | Reason |
|----------|-------------|--------|
| Local components, large files | Reference | Efficiency |
| Remote/networked components | Inline | No shared filesystem |
| Small data (<100KB) | Inline | Simplicity |
| Large data (>1MB) | Reference | Message size limits |
| Real-time streaming | Reference + notify | Latency |

### Request with Data

Requests can also include data payloads:

```json
{
  "device": "tts",
  "action": "speak",
  "request_id": "dawn_main_5",
  "data": {
    "type": "audio/wav",
    "encoding": "base64",
    "content": "UklGRiQAAABXQVZFZm10IBAAAA..."
  }
}
```

## Request ID Format

Request IDs should be unique and include enough context for debugging:

```
<component>_<context>_<sequence>
```

Examples:
- `dawn_worker0_42` - Dawn worker thread 0, sequence 42
- `dawn_main_1` - Dawn main thread, sequence 1
- `webui_session5_3` - WebUI session 5, sequence 3
- `mirage_hud_12` - Mirage HUD, sequence 12

Components must echo the exact `request_id` received - no modification.

## Topic Conventions (MQTT)

| Topic | Purpose | Publisher | Subscribers |
|-------|---------|-----------|-------------|
| `dawn` | Commands to Dawn, responses from other components | Mirage, Echo | Dawn |
| `hud` | Commands and events to Mirage HUD | Dawn | Mirage |
| `hud/status` | Mirage presence (online/offline) | Mirage | Dawn |
| `hud/discovery/#` | HUD capability discovery | Mirage | Dawn |
| `dawn/status` | Dawn presence (online/offline) | Dawn | Mirage, Reachy bridge |
| `dawn/events` | What Dawn's voice interface is doing (see below) | Dawn | Reachy bridge |
| `echo/cmd` | Commands to Echo modem daemon | Dawn | Echo |
| `echo/events` | Unsolicited events from Echo (calls, SMS) | Echo | Dawn |
| `echo/response` | Command responses from Echo | Echo | Dawn |
| `echo/status` | Echo presence (online/offline) | Echo | Dawn |
| `echo/telemetry` | Modem metrics (signal, registration) | Echo | Dawn, Mirage |
| `stat/telemetry` | System telemetry (battery, CPU, fan) | Stat | Mirage |
| `stat/status` | Stat presence (online/offline) | Stat | Dawn, Mirage |
| `reachy/status` | Reachy Mini bridge presence (online/offline) | Reachy bridge | Dawn |
| `oasis/broadcast` | System-wide announcements | Any | All components |

Responses are sent to the topic associated with the requesting component.

### Voice Interface Events (`dawn/events`)

Dawn publishes what its voice interface is doing, for components that give it
a body or a face.  QoS 0, not retained: a subscriber that starts mid-session
learns the state from the next event.  Every message is an event:

```json
{
  "device": "dawn",
  "msg_type": "event",
  "event": "state_change",
  "surface": "local",
  "timestamp": 1729700000000,
  "state": "PROCESS_COMMAND"
}
```

| `event` | Fields | Meaning |
|---------|--------|---------|
| `state_change` | `state`: `SILENCE`, `WAKEWORD_LISTEN`, `COMMAND_RECORDING`, `PROCESS_COMMAND` | The voice state machine changed state (`PROCESS_COMMAND` while a reply is being generated) |
| `wake_word` | `with_command`: bool | The wake word was heard (no `state_change` marks this) |
| `speaking_start` | — | Speech started playing on the local speaker |
| `speaking_end` | `interrupted`: bool | That speech finished, or was cut off |
| `expression` | `name`: one of `happy`, `excited`, `curious`, `surprised`, `confused`, `concerned`, `sad`, `apologetic`, `calm`, `tired` | The sentence starting now carries this emotion |

`surface` names the interface the event came from; only `local` (Dawn's own
microphone and speaker) publishes today.  `state_change` is also published on
`hud`, unchanged, for Mirage.

## Protocol Rules

### For Request Senders

1. Generate unique `request_id` if you need a response
2. Set reasonable timeout (recommended: 5-10 seconds for local operations)
3. Handle both success and error responses
4. Handle timeout (no response) gracefully
5. Choose reference vs inline data based on context

### For Request Receivers

1. **Always echo `request_id`** if present in the request
2. Send response to the appropriate topic (sender's topic)
3. Include `status: "error"` and `error` object on failure
4. Respond even on failure - silence causes sender timeout
5. Support both reference and inline data when practical

### Backward Compatibility

- Receivers must handle messages without `request_id` (legacy behavior)
- Receivers must ignore unknown fields
- Senders should not require `status` field (assume success if absent)
- Receivers should support both `value` (reference) and `data` (inline)

## Discovery Messages

Discovery messages enable components to advertise their capabilities at runtime. Unlike request/response patterns, discovery uses a publish/subscribe model with retained messages.

### Purpose

- **Dynamic capability advertisement**: Components publish what features/modes they support
- **Hot-reload**: Subscribers update their configuration when capabilities change
- **Decoupling**: Publishers and subscribers don't need to know about each other

### Topic Pattern

```
<component>/discovery/<capability>
```

Examples:
- `hud/discovery/elements` - Available HUD elements
- `hud/discovery/modes` - Available HUD screen modes
- `audio/discovery/devices` - Available audio devices (future)

### Discovery Message Format

```json
{
  "device": "mirage",
  "msg_type": "discovery",
  "timestamp": 1706644800000,
  "<capability>": ["item1", "item2", "item3"]
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `device` | Yes | Component publishing the discovery |
| `msg_type` | Yes | Must be `"discovery"` |
| `timestamp` | Yes | Unix milliseconds when published |
| `<capability>` | Yes | Array of available items (field name matches capability) |

### Discovery Request Format

Components can request republication of discovery messages:

```json
{
  "device": "dawn",
  "msg_type": "discovery_request",
  "timestamp": 1706644800
}
```

**Topic**: `<component>/discovery/request`

Publishers should subscribe to their request topic and republish all discovery messages when received.

### Discovery Examples

**HUD Elements Discovery** (Mirage → `hud/discovery/elements`):
```json
{
  "device": "mirage",
  "msg_type": "discovery",
  "timestamp": 1706644800000,
  "elements": ["armor_display", "detect", "map", "info"]
}
```

**HUD Modes Discovery** (Mirage → `hud/discovery/modes`):
```json
{
  "device": "mirage",
  "msg_type": "discovery",
  "timestamp": 1706644800000,
  "huds": ["default", "armor", "environmental", "automotive"]
}
```

**Discovery Request** (Dawn → `hud/discovery/request`):
```json
{
  "device": "dawn",
  "msg_type": "discovery_request",
  "timestamp": 1706644800
}
```

### Discovery Protocol Rules

**For Publishers:**
1. Publish discovery messages with `retain=true` (MQTT)
2. Republish on startup and when capabilities change
3. Subscribe to `<component>/discovery/request` and republish when received
4. Include `timestamp` for staleness detection

**For Subscribers:**
1. Subscribe to `<component>/discovery/#` wildcard
2. Handle retained messages on initial subscription
3. Optionally publish discovery request if no retained messages received
4. Consider messages stale after configurable threshold (e.g., 5 minutes)
5. Fall back to defaults if discovery unavailable

### Discovery vs Request/Response

| Aspect | Request/Response | Discovery |
|--------|------------------|-----------|
| Correlation | Uses `request_id` | No correlation needed |
| Delivery | One-to-one | One-to-many (broadcast) |
| Persistence | Ephemeral | Retained (MQTT) |
| Timing | On-demand | Startup + on-change |
| Use case | Commands, queries | Capability advertisement |

## Component Status (Keepalive)

Component status messages enable presence detection and health monitoring. This is critical for components like Mirage (HUD) where Dawn needs to know if the hardware is available before exposing related tools to the LLM.

### Purpose

- **Presence detection**: Know when components connect/disconnect
- **Graceful degradation**: Hide tools for unavailable hardware
- **Immediate disconnect detection**: Via MQTT Last Will and Testament (LWT)
- **Network issue detection**: Via periodic heartbeat timeout

### Topic Pattern

```
<component>/status
```

Examples:
- `hud/status` - Mirage HUD presence (Mirage publishes, Dawn subscribes)
- `dawn/status` - Dawn AI assistant presence (Dawn publishes, Mirage subscribes)
- `echo/status` - Echo modem daemon presence (Echo publishes, Dawn subscribes)
- `stat/status` - Stat telemetry daemon presence (Stat publishes, Dawn/Mirage subscribe)

### Status Message Format

All status messages (LWT, online, offline, heartbeat) MUST include the four required fields below.

```json
{
  "device": "mirage",
  "msg_type": "status",
  "status": "online",
  "timestamp": 1706644800000,
  "version": "1.0.0"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `device` | Yes | Component name |
| `msg_type` | Yes | Must be `"status"` |
| `status` | Yes | `"online"` or `"offline"` |
| `timestamp` | Yes | Unix milliseconds when published |
| `version` | No | Component version string |
| `capabilities` | No | Array of supported features |

### Implementation: MQTT Last Will and Testament (LWT)

LWT provides immediate notification when a client disconnects unexpectedly (crash, network loss, etc.).

**On MQTT Connect**, set LWT:
- **Topic**: `<component>/status`
- **Payload**: `{"device": "<name>", "msg_type": "status", "status": "offline", "timestamp": 0}`
- **QoS**: 1 (at least once)
- **Retain**: true

**Immediately after connect**, publish online status:
```json
{
  "device": "mirage",
  "msg_type": "status",
  "status": "online",
  "timestamp": 1706644800
}
```
- **Retain**: true (overwrites the LWT retained message)

**On graceful disconnect**, publish offline status before disconnecting:
```json
{
  "device": "mirage",
  "msg_type": "status",
  "status": "offline",
  "timestamp": 1706644800
}
```

### Implementation: Periodic Heartbeat

Heartbeat provides backup detection for cases LWT might miss (network partitions, broker issues).

**Publisher (e.g., Mirage)**:
1. Publish status message every 30 seconds
2. Use same `<component>/status` topic
3. Set `retain=true` (updates the retained message)

**Subscriber (e.g., Dawn)**:
1. Track `timestamp` of last received status
2. Consider component offline if no message for 90 seconds (3x heartbeat interval)
3. On timeout, treat as if "offline" status received

### Heartbeat Intervals

| Interval | Value | Description |
|----------|-------|-------------|
| Publish | 30s | How often to send heartbeat |
| Timeout | 90s | How long before considering offline (3x publish) |
| Stale | 300s | Discovery data staleness threshold |

### Status Flow Example (Bidirectional)

```
┌─────────────┐                         ┌─────────────┐
│    Dawn     │                         │   Mirage    │
└──────┬──────┘                         └──────┬──────┘
       │                                       │
       │    1. Both set LWT on MQTT connect    │
       │◄── LWT: dawn/status offline           │
       │                                       │◄── LWT: hud/status offline
       │                                       │
       │    2. Both publish online (retained)  │
       │────────────────────────────────────────►│ dawn/status: online
       │◄──────────────────────────────────────│ hud/status: online
       │                                       │
       │    3. Both subscribe to peer status   │
       │──── subscribe: hud/status ────────────►│
       │◄─── subscribe: dawn/status ───────────│
       │                                       │
       │    4. Dawn enables armor tools        │
       │       Mirage shows "AI Online"        │
       ├───────────────────────────────────────┤
       │                                       │
       │    5. Heartbeats every 30s            │
       │────────────────────────────────────────►│ dawn/status: online
       │◄──────────────────────────────────────│ hud/status: online
       │                                       │
       │         ... time passes ...           │
       │                                       │
       │    6. Mirage crashes unexpectedly     │
       │                                       │ ╳
       │                                       │
       │    7. Broker publishes Mirage LWT     │
       │◄──────────────────────────────────────│ hud/status: offline
       │                                       │    (from LWT)
       │                                       │
       │    8. Dawn disables armor tools       │
       ├───────────────────────────────────────┤
       │                                       │
       ▼                                       ▼
```

### Status Protocol Rules

**For Publishers (e.g., Mirage, Dawn):**
1. Set LWT on MQTT connect with offline status
2. Publish online status immediately after connect (retained)
3. Publish heartbeat every 30 seconds (retained)
4. Publish offline status before graceful disconnect
5. Include `timestamp` in all status messages

**For Subscribers (e.g., Dawn, Mirage):**
1. Subscribe to `<component>/status` topics
2. On "online": trigger discovery request, enable related features
3. On "offline": disable related features immediately
4. Track last heartbeat timestamp
5. If no heartbeat for 90 seconds, treat as offline
6. Handle retained messages on initial subscription

### Bidirectional Status

Both Dawn and Mirage publish their status, enabling mutual awareness:

| Publisher | Topic | Subscribers | Use Case |
|-----------|-------|-------------|----------|
| Dawn | `dawn/status` | Mirage | HUD can show "AI Online/Offline" indicator |
| Mirage | `hud/status` | Dawn | Dawn enables/disables armor tools |

**Dawn Status Example:**
```json
{
  "device": "dawn",
  "msg_type": "status",
  "status": "online",
  "timestamp": 1706644800000,
  "version": "2.0.0",
  "capabilities": ["voice", "vision", "tools"]
}
```

**Mirage Status Example:**
```json
{
  "device": "mirage",
  "msg_type": "status",
  "status": "online",
  "timestamp": 1706644800000,
  "version": "1.5.0",
  "capabilities": ["hud", "camera", "recording"]
}
```

The optional `capabilities` field allows components to advertise high-level features, useful for UI indicators or conditional behavior.

### Relationship to Discovery

Status and discovery work together:

| Event | Status Action | Discovery Action |
|-------|---------------|------------------|
| Component connects | Publish "online" | Publish discovery (retained) |
| Subscriber connects | Receive retained status | Receive retained discovery |
| Component disconnects | LWT publishes "offline" | Discovery becomes stale |
| Network timeout | Subscriber treats as offline | Subscriber uses defaults |

**Important**: Discovery messages remain retained even after disconnect. Subscribers should:
1. Check status first (is component online?)
2. Only use discovery data if component is online
3. Fall back to defaults if offline

## Examples

### Viewing with File Reference

Request (Dawn → Mirage):
```json
{
  "device": "viewing",
  "action": "look",
  "value": "what do you see?",
  "request_id": "dawn_worker0_1"
}
```

Response (Mirage → Dawn):
```json
{
  "device": "viewing",
  "action": "completed",
  "value": "/home/jetson/recordings/snapshot-20231220_143052.jpg",
  "request_id": "dawn_worker0_1",
  "status": "success"
}
```

### Viewing with Inline Data

Request (Dawn → Mirage):
```json
{
  "device": "viewing",
  "action": "look",
  "value": "what do you see?",
  "request_id": "dawn_worker0_1",
  "inline_response": true
}
```

Response (Mirage → Dawn):
```json
{
  "device": "viewing",
  "action": "completed",
  "request_id": "dawn_worker0_1",
  "status": "success",
  "data": {
    "type": "image/jpeg",
    "encoding": "base64",
    "content": "/9j/4AAQSkZJRgABAQEASABIAAD...",
    "size": 145017
  }
}
```

### Weather Query

Request:
```json
{
  "device": "weather",
  "action": "get",
  "value": "Atlanta, Georgia",
  "request_id": "dawn_main_5"
}
```

Response with structured data:
```json
{
  "device": "weather",
  "action": "completed",
  "request_id": "dawn_main_5",
  "status": "success",
  "data": {
    "type": "application/json",
    "encoding": "none",
    "content": {
      "temperature_f": 72,
      "humidity_pct": 45,
      "conditions": "partly cloudy",
      "wind_mph": 8
    }
  }
}
```

### Error Case

```json
{
  "device": "homeassistant",
  "action": "completed",
  "request_id": "dawn_worker2_8",
  "status": "error",
  "error": {
    "code": "DEVICE_NOT_FOUND",
    "message": "No device named 'living room light' found"
  }
}
```

## Message Types (`msg_type`)

The `msg_type` field identifies what kind of message is being sent. It is required on shared topics where multiple message types coexist (the field disambiguates), and on status and discovery topics (as specified in their respective sections). It is recommended on dedicated topics where the topic name already implies the type, as it aids debugging, logging, and tooling.

| Value | Description | Status |
|-------|-------------|--------|
| `request` | Command or query (implicit default for messages with `action`) | Active |
| `response` | Reply to a request | Active |
| `event` | Unsolicited notification (see Event Message section) | Active |
| `discovery` | Capability advertisement | Active |
| `discovery_request` | Request for discovery republication | Active |
| `status` | Component presence/health (online/offline) | Active |
| `telemetry` | Periodic sensor/metrics data | Active |
| `progress` | Intermediate status update | Future |

### `msg_type` Requirements

| Topic type | `msg_type` | Rationale |
|------------|------------|-----------|
| Shared topics (e.g., `hud`, `dawn`) | Required | Multiple message types coexist — type field disambiguates |
| Dedicated topics (e.g., `echo/events`, `stat/telemetry`) | Recommended | Topic name implies type, but field aids tooling/logging |
| Status topics (`*/status`) | Required | Specified in Component Status section |
| Discovery topics (`*/discovery/*`) | Required | Specified in Discovery Messages section |

## Future Extensions

These fields are reserved for future use:

| Field | Purpose |
|-------|---------|
| `ocp_version` | Protocol version for breaking changes |
| `reply_to` | Explicit response topic override |
| `correlation_id` | For multi-message sequences |
| `ttl` | Time-to-live for message expiration |
| `priority` | Message priority level |

## Implementation Checklist

### Mirage (HUD)
- [x] Extract `request_id` from incoming commands
- [x] Pass `request_id` through command processing chain
- [x] Echo `request_id` in all response messages
- [x] Add `status` field to responses
- [x] Send error responses instead of silence on failure
- [x] Support inline base64 data responses (config-driven via `Vision Inline Data`)
- [x] Add `timestamp` (ms) to outbound response messages
- [x] Add `checksum` to file reference responses
- [x] Add `checksum` to inline data responses
- [x] TTS command: use json-c for JSON construction, add `timestamp` (ms)
- [x] Status/discovery timestamps in milliseconds
- [x] Subscribe to `stat/telemetry` + `stat/status` (update from `stat` topic)
- [x] Update stat parser: route on `type` field instead of `device`

### Dawn
- [x] Use `command_router` as single path for correlated requests
- [x] Generate unique `request_id` for outbound commands
- [x] Add `status` and `error` handling to response parsing
- [x] Support receiving both reference and inline data (viewing responses)
- [x] Add `timestamp` to outbound request messages (sync commands via `ocp_get_timestamp_ms()`)
- [x] Validate `checksum` when present in file reference responses
- [x] Validate `checksum` when present in inline data responses
- [x] HUD event publishes: include `event` field + `msg_type: "event"` (required — shared `hud` topic)
- [x] AI state publish: add `event` field, `msg_type: "event"`, `timestamp` (ms)
- [x] Phone service: timestamps in milliseconds via `ocp_get_timestamp_ms()`
- [x] MQTT-only tool publish: add `timestamp` (ms)
- [x] Voice interface events on `dawn/events` (`msg_type: "event"`, `surface`)
- [x] Subscribe to `reachy/status`; peer status topics re-subscribed on every connect

### Echo (Modem Daemon)
- [x] Response format: `device`, `action`, `request_id`, `status`, nested `error`
- [x] Echo `request_id` in all responses
- [x] Use `event` field in event messages (per v1.4 Event Message spec)
- [x] Include `msg_type: "event"` in event messages
- [x] Status messages: `device`, `msg_type: "status"`, `status`, `timestamp` (ms)
- [x] LWT with `msg_type: "status"`, `timestamp: 0`
- [x] Timestamps in Unix milliseconds

### Stat (Telemetry Daemon)
- [x] Use `device: "stat"` as component identity (not sub-type name)
- [x] Include `msg_type: "telemetry"` in telemetry messages
- [x] Include `type` field for sub-device discriminator (Battery, SystemMetrics, Fan, etc.)
- [x] Include `timestamp` (ms) in all messages
- [x] Publish on `stat/telemetry` topic
- [x] Status messages on `stat/status`: `device`, `msg_type: "status"`, `status`, `timestamp`
- [x] LWT with `msg_type: "status"`, `timestamp: 0`

### Reachy Mini Bridge
- [x] Status messages on `reachy/status`: `device: "reachy"`, `msg_type: "status"`, `status`, `timestamp` (ms)
- [x] LWT with `msg_type: "status"`, `timestamp: 0`; heartbeat every 30 seconds
- [x] Subscribe to `dawn/events` and `dawn/status`; act only on `device: "dawn"` events

### Future Components
- [ ] Implement OCP handling from the start (request/response, events, status)
- [ ] Use consistent `request_id` format
- [ ] Always respond (success or error) to requests with `request_id`
- [ ] Support both data transport modes when appropriate
- [ ] Use `msg_type` on all published messages
- [ ] Timestamps in Unix milliseconds

### Discovery (HUD)
- [ ] Mirage: Publish `hud/discovery/elements` on startup (retained)
- [ ] Mirage: Publish `hud/discovery/modes` on startup (retained)
- [ ] Mirage: Subscribe to `hud/discovery/request`, republish on receive
- [x] Dawn: Subscribe to `hud/discovery/#`
- [x] Dawn: Update tool parameters from discovery messages
- [x] Dawn: Publish discovery request on connect
- [x] Dawn: Fall back to defaults if no discovery within timeout
- [x] Dawn: Disable armor tools until discovery succeeds

### Component Status / Keepalive (Mirage → Dawn)
- [ ] Mirage: Set LWT on MQTT connect (`hud/status` offline, retained)
- [ ] Mirage: Publish online status after connect (retained)
- [ ] Mirage: Publish heartbeat every 30 seconds
- [ ] Mirage: Publish offline status before graceful disconnect
- [ ] Dawn: Subscribe to `hud/status`
- [ ] Dawn: Enable armor tools on "online" status
- [ ] Dawn: Disable armor tools on "offline" status
- [ ] Dawn: Track heartbeat timestamp, timeout after 90s
- [ ] Dawn: Integrate status with discovery (only use discovery if online)

### Component Status / Keepalive (Dawn → Mirage)
- [ ] Dawn: Set LWT on MQTT connect (`dawn/status` offline, retained)
- [ ] Dawn: Publish online status after connect (retained)
- [ ] Dawn: Publish heartbeat every 30 seconds
- [ ] Dawn: Publish offline status before graceful disconnect
- [ ] Mirage: Subscribe to `dawn/status`
- [ ] Mirage: Show "AI Online/Offline" indicator on HUD
- [ ] Mirage: Track heartbeat timestamp, timeout after 90s

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-12-20 | Initial specification |
| 1.1 | 2025-12-27 | Added `timestamp` field (optional, for debugging). Standardized checksum to SHA256. Added `checksum` to file reference and inline data responses. Defined supported encodings: base64, utf8, none. Implemented in both Dawn (validation) and Mirage (generation). |
| 1.2 | 2026-01-30 | Added Discovery Messages section for capability advertisement. Defined `msg_type` values: discovery, discovery_request. Standardized topic pattern `<component>/discovery/<capability>`. |
| 1.3 | 2026-01-30 | Added Component Status (Keepalive) section. Defined MQTT LWT for immediate disconnect detection. Defined periodic heartbeat (30s publish, 90s timeout). Added `msg_type: status` with online/offline values. Standardized topic pattern `<component>/status`. |
| 1.4 | 2026-04 | Added Event Message type with `event` field (distinct from `action` — indicative vs imperative). Promoted `msg_type` from future to standard field with required/recommended guidance per topic type. Added `telemetry` message type. Codified minimum required fields for Component Status messages. Added Echo and Stat topics to Topic Conventions. Added Echo and Stat to Implementation Checklist. Updated all examples to use millisecond timestamps. |
| 1.5 | 2026-10 | Added `dawn/events` (voice interface events: state, wake word, speaking, expression) and `reachy/status` (Reachy Mini bridge presence). |
