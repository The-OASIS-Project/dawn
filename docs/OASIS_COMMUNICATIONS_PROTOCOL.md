# OASIS Communications Protocol (OCP) v1.0

A standardized messaging protocol for inter-component communication within The OASIS Project.

## Overview

The OASIS Communications Protocol defines a consistent message format for request/response communication between OASIS components (Dawn, Mirage, and future modules). The protocol ensures proper correlation of requests with responses, standardized error handling, flexible data transport, and extensibility for future needs.

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
| `device` | Yes | Target device/capability (e.g., "viewing", "weather", "smartthings") |
| `action` | Yes | Action to perform (e.g., "look", "get", "on", "off") |
| `value` | No | Action parameter (string, meaning depends on device/action) |
| `request_id` | No* | Unique identifier for correlating response. *Required if response needed |
| `session_id` | No | Client session ID for per-session context |
| `data` | No | Binary/structured data payload (see Data Transport section) |

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
  "status": "success"
}
```

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
| `encoding` | Yes | "base64" for binary, "utf8" for text, "none" for JSON objects |
| `content` | Yes | The encoded data |
| `size` | No | Original size in bytes (for validation) |
| `checksum` | No | MD5 or SHA256 hash (for integrity) |

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

| Topic | Purpose | Subscribers |
|-------|---------|-------------|
| `dawn` | Commands to Dawn, responses from other components | Dawn |
| `hud` | Commands to Mirage HUD | Mirage |
| `oasis/broadcast` | System-wide announcements | All components |
| `oasis/<component>` | Future per-component topics | Specific component |

Responses are sent to the topic associated with the requesting component.

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
  "device": "smartthings",
  "action": "completed",
  "request_id": "dawn_worker2_8",
  "status": "error",
  "error": {
    "code": "DEVICE_NOT_FOUND",
    "message": "No device named 'living room light' found"
  }
}
```

## Future Extensions

These fields are reserved for future use:

| Field | Purpose |
|-------|---------|
| `ocp_version` | Protocol version for breaking changes |
| `timestamp` | Unix milliseconds for timing/ordering |
| `reply_to` | Explicit response topic override |
| `msg_type` | "request", "response", "progress", "event" |
| `correlation_id` | For multi-message sequences |
| `ttl` | Time-to-live for message expiration |
| `priority` | Message priority level |

## Implementation Checklist

### Mirage (HUD)
- [ ] Extract `request_id` from incoming commands
- [ ] Pass `request_id` through command processing chain
- [ ] Echo `request_id` in all response messages
- [ ] Add `status` field to responses
- [ ] Send error responses instead of silence on failure
- [ ] Support `inline_response` flag for returning base64 data

### Dawn
- [ ] Use `command_router` as single path for correlated requests
- [ ] Remove duplicate vision data paths
- [ ] Add `status` and `error` handling to response parsing
- [ ] Remove polling workarounds once Mirage supports `request_id`
- [ ] Support receiving both reference and inline data

### Future Components
- [ ] Implement OCP request/response handling from the start
- [ ] Use consistent `request_id` format
- [ ] Always respond (success or error) to requests with `request_id`
- [ ] Support both data transport modes when appropriate

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-12-20 | Initial specification |
