# DAWN WebUI Design Document

## Overview

This document describes the design for DAWN's web-based user interface, enabling browser-based
interaction with the voice assistant. The WebUI provides real-time visualization, live transcripts,
and both voice and text input options.

## Prerequisites

**Must complete before starting WebUI:**

- [x] Multi-Threaded Core Phase 4: Robustness + Metrics ✅ **COMPLETE**
  - ✅ Per-session LLM cancellation (abort on disconnect)
  - ✅ LLM timeout via CURL progress callback (30s)
  - ✅ Worker crash recovery
  - ✅ Metrics: active workers, session count, LLM latency
  - ✅ `text_to_speech_to_pcm()` for efficient Opus encoding

Phase 4 ensures that WebUI client disconnects don't leave workers stuck waiting for LLM responses.

**Ready to start WebUI implementation.**

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        DAWN WebUI Architecture                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  Browser                           DAWN Daemon                       │
│  ┌─────────────────────┐          ┌─────────────────────────────┐  │
│  │   Static HTML/CSS   │    ←─────│  libwebsockets (:3000)      │  │
│  │   + Vanilla JS      │          │  ├── HTTP: static files     │  │
│  └─────────────────────┘          │  └── WebSocket: /ws         │  │
│          │                        └─────────────────────────────┘  │
│          ↓                                   │                       │
│  ┌─────────────────────┐                     ↓                       │
│  │  WebSocket Client   │          ┌─────────────────────┐          │
│  │   (native WS API)   │  ←──────→│  WebUI Thread       │          │
│  └─────────────────────┘          │  (lws event loop)   │          │
│          │                        └─────────────────────┘          │
│          ↓                                   │                       │
│  ┌─────────────────────┐                     ↓                       │
│  │ Web Audio API       │   binary  ┌─────────────────────┐          │
│  │ (Opus via libopus)  │  ──────→  │   Worker Pool       │          │
│  └─────────────────────┘           │   (SESSION_TYPE_    │          │
│                                    │    WEBSOCKET)       │          │
│                                    └─────────────────────┘          │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Design Principles

1. **Lightweight** - No Node.js, no build step, minimal dependencies
2. **Single library** - libwebsockets handles both HTTP and WebSocket (unified event loop)
3. **Embedded-friendly** - Static files can be compiled into binary
4. **Offline-capable** - Works with local LLM, no cloud dependency
5. **Mobile-responsive** - Works on phones and tablets
6. **Reuses existing infrastructure** - Worker pool, session manager

---

## Threading Model

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Thread Architecture                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│  MAIN THREAD                         WEBUI THREAD                             │
│  ───────────                         ────────────                             │
│  ┌─────────────────────┐            ┌─────────────────────┐                  │
│  │ Local Audio Capture │            │ lws_service() loop  │                  │
│  │ State Machine       │            │ ├── HTTP requests   │                  │
│  │ Local LLM Thread    │            │ ├── WS callbacks    │                  │
│  └─────────────────────┘            │ └── Audio assembly  │                  │
│                                     └──────────┬──────────┘                  │
│                                                │                              │
│                                     Audio complete / Text received            │
│                                                │                              │
│                                                ▼                              │
│                                     ┌─────────────────────┐                  │
│                                     │ worker_pool_assign  │                  │
│                                     │ _client()           │                  │
│                                     └──────────┬──────────┘                  │
│                                                │                              │
│           ┌────────────────────────────────────┼────────────────────────┐    │
│           ▼                        ▼           ▼           ▼            │    │
│  ╔════════════════╗      ╔════════════════╗  ...      ╔════════════════╗│    │
│  ║   WORKER 0     ║      ║   WORKER 1     ║           ║   WORKER N     ║│    │
│  ╠════════════════╣      ╠════════════════╣           ╠════════════════╣│    │
│  ║ Opus decode    ║      ║ Opus decode    ║           ║ Opus decode    ║│    │
│  ║ Vosk ASR       ║      ║ Vosk ASR       ║           ║ Vosk ASR       ║│    │
│  ║ LLM call       ║      ║ LLM call       ║           ║ LLM call       ║│    │
│  ║ Piper TTS      ║      ║ Piper TTS      ║           ║ Piper TTS      ║│    │
│  ║ Opus encode    ║      ║ Opus encode    ║           ║ Opus encode    ║│    │
│  ╚═══════╤════════╝      ╚═══════╤════════╝           ╚═══════╤════════╝│    │
│          │                       │                            │         │    │
│          └───────────────────────┴────────────────────────────┘         │    │
│                                  │                                       │    │
│                                  ▼                                       │    │
│                       ┌─────────────────────┐                            │    │
│                       │ WebSocket send      │  (response audio/text)     │    │
│                       │ (back to WEBUI thd) │                            │    │
│                       └─────────────────────┘                            │    │
│                                                                               │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Thread Responsibilities

| Thread | Responsibility | Notes |
|--------|---------------|-------|
| Main Thread | Local audio, state machine | Unchanged from current |
| WebUI Thread | HTTP serving, WebSocket I/O, audio chunk assembly | Dedicated thread with lws event loop |
| Worker Threads | Full pipeline: Opus→ASR→LLM→TTS→Opus | Per-worker Opus codec contexts |

### Audio Data Flow

1. **Browser** → WebSocket binary frame (raw Opus packets from Web Audio API)
2. **WebUI Thread** → Buffers Opus frames until end-of-utterance marker
3. **WebUI Thread** → Assigns to worker via `worker_pool_assign_client()`
4. **Worker Thread** → Decodes Opus (per-worker decoder), runs ASR/LLM/TTS, encodes response
5. **Worker Thread** → Sends binary response back through WebSocket

---

## Technology Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| HTTP + WebSocket | libwebsockets | Single library, unified event loop, HTTP + WS in one |
| Frontend | Vanilla JS + CSS | Zero build step, <100KB total, no npm/webpack |
| Audio Codec | Opus (libopus) | Browser-native encoding, excellent for voice (~64kbps) |
| Visualization | SVG + CSS | No Three.js dependency, hardware-accelerated |

**Why libwebsockets-only?** Originally considered libmicrohttpd + libwebsockets, but:
- Two libraries = two event loops = integration complexity
- libwebsockets has built-in HTTP serving (`LWS_CALLBACK_HTTP`)
- Single event loop simplifies threading model
- Reduces dependency count

### Dependency Additions

```cmake
# CMakeLists.txt additions
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBSOCKETS REQUIRED libwebsockets)
pkg_check_modules(OPUS REQUIRED opus)

# Compile-time option
option(DAWN_WEBUI "Enable WebUI support" ON)
```

### Installation (Debian/Ubuntu)

```bash
sudo apt install libwebsockets-dev libopus-dev
```

---

## Jarvis Analysis (Reference Implementation)

The Jarvis project (~/code/Jarvis/Jarvis-main) provides a visually impressive voice assistant UI.
Analysis of what to borrow vs. avoid:

### Borrow from Jarvis

1. **FFT Visualization Approach**
   - SVG-based circular FFT bars
   - CSS transforms for ring rotation
   - Smooth animation via requestAnimationFrame
   - Color scheme: Cyan (#22d3ee) on dark background

2. **State Machine UI Pattern**
   - States: idle → listening → active → error
   - Visual indicators for each state
   - Graceful transitions

3. **Ring Animation CSS**
   ```css
   /* Adapted from JarvisAssistant.tsx */
   .ring {
     filter: drop-shadow(0 0 20px rgba(34, 211, 238, 0.6));
     transition: opacity 300ms, transform 200ms;
   }
   ```

### Don't Use from Jarvis

| Jarvis Feature | Why Not | DAWN Alternative |
|----------------|---------|------------------|
| Node.js/Next.js | 80+ MB runtime, overkill | libmicrohttpd (~50KB) |
| OpenAI Realtime API | Cloud-only, no offline | Local Vosk + Piper |
| WebRTC data channels | Complex for our use case | Simple WebSocket |
| React + TailwindCSS | Build step required | Vanilla JS + CSS |
| Three.js | Heavy for simple visualization | SVG + CSS animations |

---

## WebSocket Protocol

### Message Format: Hybrid Binary/Text

Audio uses **binary WebSocket frames** (no base64 overhead). Control messages use **text frames** (JSON).

```
Binary Frame (audio):
┌──────────┬──────────┬─────────────────────┐
│ msg_type │  flags   │     payload         │
│ (1 byte) │ (1 byte) │   (Opus data)       │
└──────────┴──────────┴─────────────────────┘

Text Frame (control):
{ "type": "state", "payload": { ... } }
```

### Binary Message Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `AUDIO_IN` | 0x01 | Client→Server | Opus audio chunk |
| `AUDIO_IN_END` | 0x02 | Client→Server | End of utterance marker |
| `AUDIO_OUT` | 0x11 | Server→Client | TTS Opus audio chunk |
| `AUDIO_OUT_END` | 0x12 | Server→Client | End of response audio |

### Text Message Types (JSON)

#### Server → Client

| Type | Payload | Description |
|------|---------|-------------|
| `state` | `{ "state": "listening" }` | State machine update |
| `transcript` | `{ "role": "user", "text": "..." }` | ASR or LLM text |
| `metrics` | `{ "workers": 2, "sessions": 3, "latency_ms": 150 }` | System metrics |
| `error` | `{ "code": "ASR_FAILED", "message": "...", "recoverable": true }` | Error notification |
| `session` | `{ "token": "abc123..." }` | Session token for reconnection |

#### Client → Server

| Type | Payload | Description |
|------|---------|-------------|
| `text` | `{ "text": "..." }` | Text input |
| `config` | `{ "volume": 80 }` | Settings update |
| `cancel` | `{}` | Cancel current operation |
| `reconnect` | `{ "token": "abc123..." }` | Restore previous session |

### Error Codes

| Code | Description | Recoverable |
|------|-------------|-------------|
| `ASR_FAILED` | Speech recognition failed | Yes |
| `LLM_TIMEOUT` | LLM request timed out | Yes |
| `LLM_ERROR` | LLM returned error | Yes |
| `TTS_FAILED` | Text-to-speech failed | Yes |
| `SESSION_LIMIT` | Max concurrent sessions reached | No |
| `INVALID_MESSAGE` | Malformed message received | Yes |

### State Machine

```
    ┌──────────────────────────────────────────┐
    │                                          │
    ▼                                          │
┌───────┐    audio/text     ┌──────────┐      │
│ IDLE  │ ─────────────────→│ THINKING │      │
└───────┘                   └──────────┘      │
    ▲                            │            │
    │                            ▼            │
    │                       ┌──────────┐      │
    │                       │ SPEAKING │ ─────┘
    │                       └──────────┘
    │                            │
    │         error              │
    └────────────────────────────┘
```

### Pre-Serialized State Messages

For broadcast efficiency, common state messages are pre-built:

```c
static const char *state_idle_msg = "{\"type\":\"state\",\"payload\":{\"state\":\"idle\"}}";
static const char *state_thinking_msg = "{\"type\":\"state\",\"payload\":{\"state\":\"thinking\"}}";
static const char *state_speaking_msg = "{\"type\":\"state\",\"payload\":{\"state\":\"speaking\"}}";
```

---

## File Structure

### Backend (C)

```
include/webui/
├── webui_server.h          # Unified HTTP + WebSocket interface
└── webui_audio.h           # Per-worker Opus codec interface

src/webui/
├── webui_server.c          # libwebsockets HTTP + WebSocket handling
└── webui_audio.c           # Opus encode/decode (per-worker contexts)
```

### Frontend (Static)

```
www/
├── index.html              # Single page application
├── css/
│   └── dawn.css            # Styling (Jarvis-inspired dark theme)
├── js/
│   ├── dawn.js             # Main application logic
│   ├── websocket.js        # WebSocket client wrapper
│   ├── audio.js            # MediaRecorder + AudioContext
│   └── visualizer.js       # FFT ring visualization
└── assets/
    ├── ring1.svg           # Outer ring graphic
    ├── ring2.svg           # Inner ring graphic
    └── logo.svg            # DAWN/FRIDAY logo
```

### Static File Serving Options

1. **Filesystem** (development): Serve from `/var/lib/dawn/www/` or config path
2. **Embedded** (production): Compile into binary with gzip pre-compression

```c
// Embedded static files with gzip compression (~70% size reduction)
extern const unsigned char index_html_gz[];
extern const unsigned int index_html_gz_len;
// Server sets Content-Encoding: gzip header

// CMake generates embedded files:
// add_custom_command(
//     OUTPUT ${CMAKE_BINARY_DIR}/generated/webui_files.c
//     COMMAND gzip -9 -c www/index.html > www/index.html.gz
//     COMMAND xxd -i www/index.html.gz > ${CMAKE_BINARY_DIR}/generated/webui_files.c
//     DEPENDS www/index.html
// )
```

**Memory trade-off:**
- Filesystem: ~0 bytes RAM, but requires SD card I/O
- Embedded: ~30-40KB RAM (gzipped), instant access, no filesystem dependency

---

## Implementation Phases

### Phase 1: HTTP + WebSocket Server Foundation (1-2 days)

**Goal:** Serve static files, establish WebSocket infrastructure

```c
// include/webui/webui_server.h

#define WEBUI_DEFAULT_PORT 3000  // "I love you 3000"
#define WEBUI_DEFAULT_WWW_PATH "/var/lib/dawn/www"
#define WEBUI_MAX_CLIENTS 4  // Leave room for local + DAP clients
#define WEBUI_SUBPROTOCOL "dawn-1.0"  // Protocol versioning

/**
 * @brief Initialize WebUI server (HTTP + WebSocket via libwebsockets)
 * @param port Port to listen on (0 for default)
 * @param www_path Path to static files (NULL for default/embedded)
 * @return 0 on success, 1 on failure
 *
 * @note Spawns dedicated WebUI thread running lws_service() loop
 */
int webui_server_init(int port, const char *www_path);

/**
 * @brief Shutdown WebUI server and join thread
 */
void webui_server_shutdown(void);

/**
 * @brief Check if WebUI is enabled and running
 */
bool webui_is_enabled(void);

/**
 * @brief Get current WebSocket client count
 */
int webui_client_count(void);
```

**Deliverables:**
- [x] libwebsockets integration (HTTP + WebSocket in single library) ✅
- [x] Dedicated WebUI thread with `lws_service()` event loop ✅
- [x] Static file serving with MIME types and gzip support ✅
- [x] WebSocket subprotocol registration (`dawn-1.0`) ✅
- [x] Basic index.html placeholder ✅
- [x] Config options: `webui.enabled`, `webui.port`, `webui.www_path`, `webui.max_clients` ✅

**Phase 1 COMPLETE** (2025-12-16)

### Phase 2: Session Integration + Message Handling (2-3 days)

**Goal:** Connect WebSocket clients to session manager and worker pool

```c
// Binary message type constants (match protocol spec)
#define WS_BIN_AUDIO_IN      0x01
#define WS_BIN_AUDIO_IN_END  0x02
#define WS_BIN_AUDIO_OUT     0x11
#define WS_BIN_AUDIO_OUT_END 0x12

/**
 * @brief Per-WebSocket connection state (stored in lws user data)
 *
 * @ownership WebUI thread owns this structure
 * @note audio_buffer ownership transfers to worker on AUDIO_IN_END
 */
typedef struct {
   struct lws *wsi;              // libwebsockets handle (for response routing)
   session_t *session;           // Session manager reference
   char session_token[33];       // Reconnection token (32 hex chars + null)
   uint8_t *audio_buffer;        // Accumulates compressed Opus frames (~16KB for 2s)
   size_t audio_buffer_len;
   size_t audio_buffer_capacity;
} ws_connection_t;
```

#### WebSocket-to-Session Binding (C1)

WebSocket connections use `lws *wsi` handles instead of file descriptors. The binding works as follows:

```c
// libwebsockets callback handler
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
   ws_connection_t *conn = (ws_connection_t *)user;

   switch (reason) {
   case LWS_CALLBACK_ESTABLISHED:
      // New WebSocket connection - create session
      conn->wsi = wsi;
      conn->session = session_create(SESSION_TYPE_WEBSOCKET, -1);  // fd=-1 for WS
      if (!conn->session) {
         // Session limit reached
         webui_send_error_immediate(wsi, "SESSION_LIMIT", "Max sessions reached");
         return -1;  // Close connection
      }
      // Store wsi in session for response routing
      conn->session->client_data = conn;
      // Generate session token for reconnection
      webui_generate_session_token(conn->session_token);
      webui_store_token_mapping(conn->session_token, conn->session);
      // Send token to client
      webui_send_session_token(conn, conn->session_token);
      break;

   case LWS_CALLBACK_CLOSED:
      // WebSocket disconnected
      if (conn->session) {
         conn->session->disconnected = true;  // Signal worker to abort
         webui_remove_token_mapping(conn->session_token);
         session_destroy(conn->session->session_id);
      }
      // Free any pending audio buffer (client disconnected mid-utterance)
      free(conn->audio_buffer);
      conn->audio_buffer = NULL;
      break;

   case LWS_CALLBACK_RECEIVE:
      // Handle incoming message (see message handling below)
      break;
   }
   return 0;
}
```

#### Session Token Specification

```c
// Session token: 128 bits of cryptographic randomness, hex-encoded
// Storage: Hash map from token -> session pointer
// Expiration: Tokens expire with session (SESSION_TIMEOUT_SEC = 1800s / 30 min)

#include <sys/random.h>

void webui_generate_session_token(char token_out[33]) {
   uint8_t random_bytes[16];
   getrandom(random_bytes, 16, 0);  // Linux getrandom()
   for (int i = 0; i < 16; i++) {
      sprintf(&token_out[i * 2], "%02x", random_bytes[i]);
   }
   token_out[32] = '\0';
}

// Token lookup (simple linear scan for ≤8 sessions)
static struct {
   char token[33];
   session_t *session;
} token_map[MAX_SESSIONS];

session_t *webui_lookup_token(const char *token) {
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (token_map[i].session &&
          !token_map[i].session->disconnected &&
          strcmp(token_map[i].token, token) == 0) {
         return token_map[i].session;
      }
   }
   return NULL;  // Token not found or session disconnected
}
```

#### Worker Pool Entry Point for WebSocket (C2)

WebSocket clients don't have a file descriptor for the worker to read from. Instead, the WebUI thread
pre-buffers audio and passes the complete buffer to the worker:

```c
// include/core/worker_pool.h addition

/**
 * @brief Work item for WebSocket clients (pre-buffered audio or text)
 *
 * Unlike DAP clients where workers read from sockets, WebSocket audio is
 * pre-buffered by the WebUI thread and passed to workers as a complete buffer.
 */
typedef struct {
   session_t *session;
   enum { WS_WORK_AUDIO, WS_WORK_TEXT } type;
   union {
      struct {
         uint8_t *opus_data;    // Ownership transfers to worker
         size_t opus_len;
      } audio;
      struct {
         char *text;            // Ownership transfers to worker
      } text;
   };
} ws_work_item_t;

/**
 * @brief Assign WebSocket work item to available worker
 *
 * @param item Work item (ownership transfers to worker pool)
 * @return 0 on success, 1 if all workers busy
 *
 * @note Unlike worker_pool_assign_client(), this passes pre-buffered data
 * @note Worker is responsible for freeing item->audio.opus_data or item->text.text
 */
int worker_pool_assign_websocket(ws_work_item_t *item);
```

**Audio Buffer Ownership Transfer:**

```c
// In WebUI thread, on receiving AUDIO_IN_END:
case WS_BIN_AUDIO_IN_END:
   if (conn->audio_buffer_len > 0) {
      // Create work item - TRANSFER ownership of audio buffer
      ws_work_item_t *item = malloc(sizeof(ws_work_item_t));
      item->session = conn->session;
      item->type = WS_WORK_AUDIO;
      item->audio.opus_data = conn->audio_buffer;  // Transfer ownership
      item->audio.opus_len = conn->audio_buffer_len;

      // WebUI thread no longer owns the buffer
      conn->audio_buffer = NULL;
      conn->audio_buffer_len = 0;
      conn->audio_buffer_capacity = 0;

      // Assign to worker (item ownership also transfers)
      if (worker_pool_assign_websocket(item) != 0) {
         // All workers busy - free and send error
         free(item->audio.opus_data);
         free(item);
         webui_queue_error(conn, "BUSY", "All workers busy, try again");
      }
   }
   break;
```

#### Cross-Thread Response Queue (C3)

**Critical:** libwebsockets is NOT thread-safe for writes. All `lws_write()` calls MUST occur from
the WebUI thread running `lws_service()`. Workers queue responses, then signal the WebUI thread.

```c
// Response queue for worker -> WebUI thread communication
typedef struct {
   session_t *session;
   enum { WS_RESP_TRANSCRIPT, WS_RESP_AUDIO, WS_RESP_ERROR } type;
   union {
      struct { char *role; char *text; } transcript;
      struct { uint8_t *data; size_t len; bool is_final; } audio;
      struct { char *code; char *message; } error;
   };
} ws_response_t;

#define WS_RESPONSE_QUEUE_SIZE 64
static ws_response_t response_queue[WS_RESPONSE_QUEUE_SIZE];
static int queue_head = 0, queue_tail = 0;
static pthread_mutex_t response_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct lws_context *lws_ctx;  // Set during init

/**
 * @brief Queue a response from worker thread (thread-safe)
 */
void webui_queue_response(ws_response_t *resp) {
   pthread_mutex_lock(&response_queue_mutex);
   // Add to queue (circular buffer)
   response_queue[queue_tail] = *resp;
   queue_tail = (queue_tail + 1) % WS_RESPONSE_QUEUE_SIZE;
   pthread_mutex_unlock(&response_queue_mutex);

   // Wake up lws_service() loop to process queue
   lws_cancel_service(lws_ctx);
}

/**
 * @brief Process queued responses (called from WebUI thread only)
 *
 * Called from LWS_CALLBACK_EVENT_WAIT_CANCELLED after lws_cancel_service()
 */
void webui_process_response_queue(void) {
   pthread_mutex_lock(&response_queue_mutex);
   while (queue_head != queue_tail) {
      ws_response_t *resp = &response_queue[queue_head];
      queue_head = (queue_head + 1) % WS_RESPONSE_QUEUE_SIZE;
      pthread_mutex_unlock(&response_queue_mutex);

      // Find connection for this session
      ws_connection_t *conn = (ws_connection_t *)resp->session->client_data;
      if (!conn || resp->session->disconnected) {
         // Client disconnected - free response data and skip
         webui_free_response(resp);
         pthread_mutex_lock(&response_queue_mutex);
         continue;
      }

      // Send via lws_write() (safe - we're in WebUI thread)
      switch (resp->type) {
      case WS_RESP_TRANSCRIPT:
         webui_send_transcript_impl(conn->wsi, resp->transcript.role, resp->transcript.text);
         free(resp->transcript.role);
         free(resp->transcript.text);
         break;
      case WS_RESP_AUDIO:
         webui_send_audio_impl(conn->wsi, resp->audio.data, resp->audio.len, resp->audio.is_final);
         free(resp->audio.data);
         break;
      case WS_RESP_ERROR:
         webui_send_error_impl(conn->wsi, resp->error.code, resp->error.message);
         free(resp->error.code);
         free(resp->error.message);
         break;
      }

      pthread_mutex_lock(&response_queue_mutex);
   }
   pthread_mutex_unlock(&response_queue_mutex);
}

// Worker-callable wrappers (queue instead of direct send)
void webui_send_transcript(session_t *session, const char *role, const char *text) {
   ws_response_t resp = {
      .session = session,
      .type = WS_RESP_TRANSCRIPT,
      .transcript = { .role = strdup(role), .text = strdup(text) }
   };
   webui_queue_response(&resp);
}

void webui_send_audio(session_t *session, const uint8_t *opus_data, size_t len, bool is_final) {
   ws_response_t resp = {
      .session = session,
      .type = WS_RESP_AUDIO,
      .audio = { .data = memdup(opus_data, len), .len = len, .is_final = is_final }
   };
   webui_queue_response(&resp);
}
```

**Deliverables:**
- [x] WebSocket connection lifecycle (LWS_CALLBACK_ESTABLISHED/CLOSED) ✅
- [x] Session binding: `session_create(SESSION_TYPE_WEBSOCKET, -1)` with `wsi` in `client_data` ✅
- [x] Session token generation (128-bit random, hex-encoded) and lookup ✅
- [x] Binary message parsing (audio) and text message parsing (JSON) ✅ (text only)
- [ ] Audio buffer accumulation with ownership transfer on `AUDIO_IN_END` *(Phase 4)*
- [ ] New `worker_pool_assign_websocket()` for pre-buffered work items *(Phase 4)*
- [x] Cross-thread response queue with `lws_cancel_service()` wakeup ✅
- [x] Worker-safe wrappers: `webui_send_transcript()`, `webui_send_audio()` ✅ (transcript/state/error)
- [x] State broadcast to all WebSocket clients ✅
- [x] Cleanup on disconnect (free pending audio buffer, remove token) ✅

**Implementation Notes (2025-12-16):**
- Text processing uses detached pthread instead of worker pool integration (simpler for text-only)
- Token-based session persistence for page refresh/reconnection
- Abandoned sessions cleaned up immediately on reconnection (not waiting for 30-min timeout)
- Command processing integrated: `<command>` tags parsed, MQTT published, follow-up LLM calls made
- Sessions not destroyed on WebSocket close (allows reconnection with preserved history)
- **History replay on reconnect**: `send_history_impl()` sends all user/assistant messages to browser
- Tool results sent to WebUI as transcript for debug display

**Phase 2 COMPLETE** (2025-12-16)

### Phase 3: Frontend MVP - Text Only (2-3 days)

**Goal:** Functional text-based interface

**index.html structure:**
```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>DAWN - Voice Assistant</title>
  <link rel="stylesheet" href="/css/dawn.css">
</head>
<body>
  <div id="app">
    <!-- Visualization container -->
    <div id="visualizer">
      <svg id="ring-outer" class="ring"></svg>
      <svg id="ring-inner" class="ring"></svg>
      <div id="logo"></div>
    </div>

    <!-- State indicator -->
    <div id="status">
      <span id="status-dot"></span>
      <span id="status-text">IDLE</span>
    </div>

    <!-- Transcript display -->
    <div id="transcript"></div>

    <!-- Input area -->
    <div id="input-area">
      <input type="text" id="text-input" placeholder="Type a message...">
      <button id="send-btn">Send</button>
      <button id="mic-btn" disabled>🎤</button>
    </div>
  </div>

  <script src="/js/websocket.js"></script>
  <script src="/js/visualizer.js"></script>
  <script src="/js/dawn.js"></script>
</body>
</html>
```

**Deliverables:**
- [x] Dark theme CSS (Jarvis-inspired) ✅
- [x] WebSocket connection management ✅
- [x] Text input/response flow ✅
- [x] State visualization (color-coded dot) ✅
- [x] Transcript display with auto-scroll ✅
- [x] Basic ring animation (rotation only, no FFT yet) ✅

**Implementation Notes (2025-12-16):**
- Single-file JS (`dawn.js`) with IIFE pattern - no build step required
- Connection status indicator in header
- Session token stored in localStorage for reconnection
- Exponential backoff with jitter for WebSocket reconnection
- **Debug mode toggle**: Checkbox shows/hides command tags and tool results
- Debug entries color-coded: cyan (commands), green (tool results), purple (other)
- Mixed text+command messages parsed: user-facing text shown normally, debug content separated
- History replayed on page refresh (entries appear immediately after reconnect)

**Phase 3 COMPLETE** (2025-12-16)

### Phase 4: Audio Pipeline (3-4 days)

**Goal:** Full voice interaction with binary WebSocket frames

**Browser Audio Flow (Web Audio API + libopus.js):**

> **Deprecation Note:** This implementation uses `ScriptProcessorNode` for initial
> compatibility. ScriptProcessor is deprecated; a future enhancement should migrate
> to `AudioWorkletNode` for better performance (separate thread, no main-thread blocking).
> Migration path: Create `opus-encoder-worklet.js` using AudioWorkletProcessor.

```javascript
// js/audio.js - Uses Web Audio API for raw PCM, libopus.js for encoding
// This avoids WebM container parsing on the server
// TODO: Migrate from ScriptProcessorNode to AudioWorkletNode (Phase 5+)

const AUDIO_CHUNK_MS = 200;  // Configurable: 200ms default (better ASR context)
const AUDIO_IN = 0x01;
const AUDIO_IN_END = 0x02;

class AudioHandler {
  constructor(websocket, config = {}) {
    this.ws = websocket;
    this.chunkMs = config.chunkMs || AUDIO_CHUNK_MS;
    this.audioContext = null;
    this.analyser = null;
    this.opusEncoder = null;
    this.scriptProcessor = null;
    this.visualizerEnabled = config.visualizer !== false;
  }

  async startRecording() {
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        channelCount: 1,
        sampleRate: 16000,
        echoCancellation: true,
        noiseSuppression: true
      }
    });

    this.audioContext = new AudioContext({ sampleRate: 16000 });
    const source = this.audioContext.createMediaStreamSource(stream);

    // Lazy FFT analyser init (only if visualization enabled)
    if (this.visualizerEnabled) {
      this.analyser = this.audioContext.createAnalyser();
      this.analyser.fftSize = 64;  // Minimum for ring visualization
      source.connect(this.analyser);
    }

    // Initialize Opus encoder (via libopus.js or similar)
    this.opusEncoder = new OpusEncoder(16000, 1, 64000);  // 16kHz mono 64kbps

    // Collect PCM samples, encode to Opus, send binary
    const bufferSize = Math.floor(16000 * this.chunkMs / 1000);  // samples per chunk
    this.scriptProcessor = this.audioContext.createScriptProcessor(bufferSize, 1, 1);

    this.scriptProcessor.onaudioprocess = (e) => {
      const pcmData = e.inputBuffer.getChannelData(0);
      const int16Data = this.float32ToInt16(pcmData);

      // Encode to Opus
      const opusFrame = this.opusEncoder.encode(int16Data);

      // Send binary frame: [msg_type][flags][opus_data]
      const frame = new Uint8Array(2 + opusFrame.length);
      frame[0] = AUDIO_IN;
      frame[1] = 0x00;  // flags (reserved)
      frame.set(opusFrame, 2);
      this.ws.send(frame.buffer);
    };

    source.connect(this.scriptProcessor);
    this.scriptProcessor.connect(this.audioContext.destination);
  }

  stopRecording() {
    // Send end-of-utterance marker
    const endFrame = new Uint8Array([AUDIO_IN_END, 0x00]);
    this.ws.send(endFrame.buffer);

    if (this.scriptProcessor) {
      this.scriptProcessor.disconnect();
      this.scriptProcessor = null;
    }
    if (this.opusEncoder) {
      this.opusEncoder.destroy();
      this.opusEncoder = null;
    }
  }

  getFFTData() {
    if (!this.visualizerEnabled || !this.analyser) return null;
    const data = new Uint8Array(this.analyser.frequencyBinCount);
    this.analyser.getByteFrequencyData(data);
    return data;
  }

  float32ToInt16(float32Array) {
    const int16 = new Int16Array(float32Array.length);
    for (let i = 0; i < float32Array.length; i++) {
      int16[i] = Math.max(-32768, Math.min(32767, float32Array[i] * 32768));
    }
    return int16;
  }
}
```

**Server Audio Processing (Per-Worker Opus Contexts):**

```c
// include/webui/webui_audio.h

#include <opus/opus.h>

/**
 * @brief Per-worker Opus codec context
 *
 * Opus encoder/decoder are STATEFUL - each worker needs its own instance
 * to avoid audio corruption during concurrent client processing.
 *
 * @thread_safety NOT thread-safe. Each worker owns its context exclusively.
 */
typedef struct {
   OpusDecoder *decoder;  // ~7KB
   OpusEncoder *encoder;  // ~12KB
} webui_opus_ctx_t;

/**
 * @brief Create Opus context for a worker
 * @return Allocated context, or NULL on error
 */
webui_opus_ctx_t *webui_opus_ctx_create(void);

/**
 * @brief Destroy Opus context
 */
void webui_opus_ctx_destroy(webui_opus_ctx_t *ctx);

/**
 * @brief Decode Opus to PCM (uses worker's decoder state)
 * @param ctx Worker's Opus context
 * @param opus_data Opus frame data
 * @param opus_len Opus frame length
 * @param pcm_out Output PCM buffer (16-bit mono)
 * @param max_samples Maximum samples to decode
 * @return Number of samples decoded, or negative on error
 */
int webui_opus_decode(webui_opus_ctx_t *ctx, const uint8_t *opus_data,
                      int opus_len, int16_t *pcm_out, int max_samples);

/**
 * @brief Encode PCM to Opus (uses worker's encoder state)
 * @param ctx Worker's Opus context
 * @param pcm_data Input PCM (16-bit mono)
 * @param pcm_samples Number of input samples
 * @param opus_out Output Opus buffer
 * @param max_opus_len Maximum output size
 * @return Opus frame size, or negative on error
 */
int webui_opus_encode(webui_opus_ctx_t *ctx, const int16_t *pcm_data,
                      int pcm_samples, uint8_t *opus_out, int max_opus_len);
```

```c
// src/webui/webui_audio.c

webui_opus_ctx_t *webui_opus_ctx_create(void) {
   webui_opus_ctx_t *ctx = calloc(1, sizeof(webui_opus_ctx_t));
   if (!ctx) return NULL;

   int err;
   ctx->decoder = opus_decoder_create(16000, 1, &err);
   if (err != OPUS_OK) {
      free(ctx);
      return NULL;
   }

   ctx->encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
   if (err != OPUS_OK) {
      opus_decoder_destroy(ctx->decoder);
      free(ctx);
      return NULL;
   }

   return ctx;
}

void webui_opus_ctx_destroy(webui_opus_ctx_t *ctx) {
   if (!ctx) return;
   if (ctx->decoder) opus_decoder_destroy(ctx->decoder);
   if (ctx->encoder) opus_encoder_destroy(ctx->encoder);
   free(ctx);
}

int webui_opus_decode(webui_opus_ctx_t *ctx, const uint8_t *opus_data,
                      int opus_len, int16_t *pcm_out, int max_samples) {
   return opus_decode(ctx->decoder, opus_data, opus_len, pcm_out, max_samples, 0);
}

int webui_opus_encode(webui_opus_ctx_t *ctx, const int16_t *pcm_data,
                      int pcm_samples, uint8_t *opus_out, int max_opus_len) {
   return opus_encode(ctx->encoder, pcm_data, pcm_samples, opus_out, max_opus_len);
}
```

**Opus Decode Error Handling Policy:**

```c
// Error handling strategy for Opus decode failures
int webui_opus_decode_with_recovery(webui_opus_ctx_t *ctx, const uint8_t *opus_data,
                                     int opus_len, int16_t *pcm_out, int max_samples) {
   int samples = opus_decode(ctx->decoder, opus_data, opus_len, pcm_out, max_samples, 0);

   if (samples < 0) {
      // Log error with Opus error string
      LOG_WARNING("Opus decode error: %s (code: %d)", opus_strerror(samples), samples);

      switch (samples) {
      case OPUS_BAD_ARG:
      case OPUS_INVALID_PACKET:
         // Packet corruption - attempt PLC (packet loss concealment)
         samples = opus_decode(ctx->decoder, NULL, 0, pcm_out, max_samples, 0);
         if (samples > 0) {
            LOG_INFO("Opus PLC generated %d samples", samples);
         }
         break;

      case OPUS_BUFFER_TOO_SMALL:
         // Caller error - return error for retry with larger buffer
         return samples;

      case OPUS_INTERNAL_ERROR:
         // Decoder state corrupted - reset decoder
         opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);
         LOG_WARNING("Opus decoder state reset");
         return 0;  // Return 0 samples, let caller continue

      default:
         // Unknown error - log and return 0
         return 0;
      }
   }

   return samples;
}
```

**Worker Pool Integration:**

```c
// Addition to worker_context_t (in worker_pool.h)
typedef struct {
   // ... existing fields ...
#ifdef DAWN_WEBUI
   webui_opus_ctx_t *opus_ctx;  // Per-worker Opus context (WebUI clients only)
#endif
} worker_context_t;

// In worker_pool_init(): create Opus context per worker (only if WebUI enabled)
#ifdef DAWN_WEBUI
for (int i = 0; i < pool_size; i++) {
   workers[i].opus_ctx = webui_opus_ctx_create();
   if (!workers[i].opus_ctx) {
      LOG_ERROR("Failed to create Opus context for worker %d", i);
      // Continue - worker can still handle DAP/text clients
   }
}
#endif
```

**Note:** The `opus_ctx` field is conditionally compiled via `DAWN_WEBUI` to avoid
linking libopus when WebUI is disabled. DAP clients use raw PCM, so they don't need Opus.

**Deliverables:**
- [ ] Web Audio API + libopus.js integration (browser)
- [ ] Binary WebSocket frame handling (no base64)
- [ ] Per-worker Opus contexts (thread-safe)
- [ ] Opus decode → PCM → Vosk ASR integration
- [ ] Piper TTS → PCM → Opus encode
- [ ] Binary audio streaming back to browser
- [ ] Configurable chunk size (default 200ms)
- [ ] FFT visualization (lazy init)
- [ ] Push-to-talk button
- [ ] VAD-based auto-stop (optional)

### Phase 5: Polish + Features (2-3 days)

**Goal:** Production-ready UI

**Reconnection with Exponential Backoff:**

```javascript
// js/websocket.js
class ReconnectingWebSocket {
  constructor(url, protocols) {
    this.url = url;
    this.protocols = protocols;
    this.reconnectAttempts = 0;
    this.maxReconnectDelay = 30000;  // 30s cap
    this.sessionToken = localStorage.getItem('dawn_session_token');
  }

  connect() {
    this.ws = new WebSocket(this.url, this.protocols);

    this.ws.onopen = () => {
      this.reconnectAttempts = 0;
      // Attempt session restoration if we have a token
      if (this.sessionToken) {
        this.ws.send(JSON.stringify({
          type: 'reconnect',
          payload: { token: this.sessionToken }
        }));
      }
    };

    this.ws.onclose = () => {
      this.scheduleReconnect();
    };
  }

  scheduleReconnect() {
    const baseDelay = 1000;  // 1s
    const delay = Math.min(
      baseDelay * Math.pow(2, this.reconnectAttempts),
      this.maxReconnectDelay
    );
    const jitter = Math.random() * 500;  // Prevent thundering herd

    setTimeout(() => {
      this.reconnectAttempts++;
      this.connect();
    }, delay + jitter);
  }

  // Called when server sends session token
  onSessionToken(token) {
    this.sessionToken = token;
    localStorage.setItem('dawn_session_token', token);
  }
}
```

**Deliverables:**
- [ ] Settings panel (volume, wake word toggle)
- [ ] Mobile responsive layout
- [ ] Connection status indicator
- [ ] Reconnection with exponential backoff + jitter
- [ ] Session token persistence (localStorage)
- [ ] Metrics display (optional debug panel)
- [ ] Keyboard shortcuts (Enter to send, Escape to cancel)
- [ ] Loading states and error handling
- [ ] Favicon and PWA manifest
- [ ] Health check endpoint (`/health`)

---

## Configuration

```toml
# dawn.toml additions

[webui]
enabled = true
port = 3000                        # "I love you 3000"
max_clients = 4                    # Leave room for local + DAP clients
www_path = "/var/lib/dawn/www"     # Optional, uses embedded if not set
audio_chunk_ms = 200               # Audio chunk size (100-500ms, default 200)
max_history_entries = 50           # Per-session conversation history limit (prevents unbounded growth)
# bind_address = "127.0.0.1"       # Default: localhost only
# ssl_cert = "/path/to/cert.pem"   # Future: HTTPS support
# ssl_key = "/path/to/key.pem"
```

```c
// include/config/dawn_config.h additions

typedef struct {
   bool enabled;
   int port;
   int max_clients;              // WEBUI_MAX_CLIENTS default: 4
   int audio_chunk_ms;           // Audio chunk size (default: 200)
   int max_history_entries;      // Per-session history limit (default: 50)
   char www_path[CONFIG_PATH_MAX];
   char bind_address[64];        // Default: "127.0.0.1"
} webui_config_t;
```

---

## Resource Constraints

### Memory Budget

| Resource | Per-Session | Notes |
|----------|-------------|-------|
| `session_t` structure | ~300 bytes | Static pool slot |
| Conversation history | ~4KB typical | json-c, unbounded growth |
| `ws_connection_t` | ~100 bytes | Audio buffer pointers |
| Audio accumulation buffer | ~16KB | 2s of Opus frames |
| libwebsockets context | ~2-4KB | Per-connection |
| **Total per WebSocket** | **~22KB** | Excluding worker resources |

| Resource | Per-Worker | Notes |
|----------|------------|-------|
| Opus decoder | ~7KB | Per-worker, not per-session |
| Opus encoder | ~12KB | Per-worker, not per-session |
| PCM decode buffer | ~64KB | 2s @ 16kHz mono 16-bit (16000×2×2) |
| **Total per worker** | **~83KB** | Created at startup |

**Total WebUI Memory Budget (4 clients, 4 workers):**
- WebSocket sessions: 4 × 22KB = ~88KB
- Workers (shared): 4 × 83KB = ~332KB (already counted in worker pool)
- Static files (embedded + gzipped): ~40KB
- **WebUI overhead: ~130KB** (excluding worker pool, which is shared with DAP)

### Buffer Sizes

```c
// webui_server.h constants
#define WEBUI_AUDIO_BUFFER_SIZE (16000 * 2 * 2)  // 2s @ 16kHz mono 16-bit = 64KB
#define WEBUI_OPUS_FRAME_MAX    4000             // Max Opus frame size
#define WEBUI_WS_TX_BUFFER      8192             // WebSocket transmit buffer
```

### CPU Budget

| Operation | Time | Frequency | Notes |
|-----------|------|-----------|-------|
| Opus decode | ~0.5ms | Per 200ms chunk | Negligible |
| Opus encode | ~1ms | Per TTS sentence | Negligible |
| JSON parse | ~0.1ms | Per control message | Text frames only |
| WebSocket I/O | ~0.1ms | Per frame | libwebsockets efficient |

### Limits

```c
#define WEBUI_MAX_CLIENTS 4          // Compile-time max
#define MAX_SESSIONS 8               // Shared with DAP (session_manager.h)
#define WORKER_POOL_MAX_SIZE 8       // Shared pool (worker_pool.h)
```

**Capacity Planning:**
- Local mic: 1 session (always reserved)
- DAP satellites: 2-3 typical
- WebUI clients: 4 max
- Total: 7-8 sessions (within MAX_SESSIONS)

---

## Security Considerations

### Phase 1 (MVP)

- **Network isolation**: Bind to localhost only by default
- **No authentication**: Intended for trusted LAN use
- **No HTTPS**: HTTP only for simplicity

### Future Enhancements

1. **HTTPS support** via libmicrohttpd SSL
2. **Basic auth** or token-based authentication
3. **Rate limiting** to prevent abuse
4. **CORS configuration** for cross-origin access

---

## Testing Strategy

### Backend Tests

1. **HTTP server**: Verify static file serving, MIME types
2. **WebSocket**: Connection lifecycle, message parsing
3. **Audio codec**: Opus encode/decode round-trip
4. **Integration**: Text input → LLM → response flow

### Frontend Tests

1. **WebSocket reconnection**: Simulate disconnects
2. **Audio recording**: Verify Opus chunks sent
3. **State transitions**: UI reflects server state
4. **Mobile**: Touch events, responsive layout

### End-to-End

1. Text query → transcript → response displayed
2. Voice query → ASR → LLM → TTS → audio playback
3. Multiple browser tabs (multiple sessions)
4. Concurrent WebUI + local mic + DAP client

---

## Estimated Effort

| Phase | Effort | Files | LOC |
|-------|--------|-------|-----|
| Phase 1: HTTP Server | 1-2 days | 2 | ~200 |
| Phase 2: WebSocket | 2-3 days | 2 | ~400 |
| Phase 3: Frontend MVP | 2-3 days | 5 | ~600 |
| Phase 4: Audio Pipeline | 3-4 days | 3 | ~400 |
| Phase 5: Polish | 2-3 days | - | ~300 |
| **Total** | **10-15 days** | **12** | **~1900** |

---

## Success Metrics

| Metric | Target |
|--------|--------|
| Page load time | < 500ms |
| WebSocket latency | < 50ms |
| Audio round-trip | < 500ms (excluding LLM) |
| Mobile usability | Touch-friendly, responsive |
| Concurrent clients | 4+ WebUI sessions |

---

## Future Enhancements

1. **Multi-room visualization**: Show all active satellites
2. **Command history**: Browsable transcript history
3. **Voice activity display**: Show who's speaking
4. **Theme customization**: User-selectable colors
5. **PWA support**: Installable web app
6. **Streaming responses**: Show LLM text as it generates

---

## Document History

- 2025-12-15: Initial design document created
- 2025-12-15: Architecture review updates:
  - Switched to libwebsockets-only (eliminates libmicrohttpd integration complexity)
  - Added threading model diagram showing audio data flow
  - Added per-worker Opus codec context pattern (thread-safe)
  - Switched to binary WebSocket frames for audio (eliminates 33% base64 overhead)
  - Added session token mechanism for reconnection
  - Added error codes enumeration
  - Added pre-serialized state messages for broadcast efficiency
- 2025-12-15: Embedded efficiency review updates:
  - Added WEBUI_MAX_CLIENTS limit (4)
  - Added Resource Constraints section with memory budget
  - Changed default audio chunk size to 200ms (configurable)
  - Added lazy FFT analyser initialization
  - Added gzip compression for embedded static files
  - Added reconnection with exponential backoff + jitter
- 2025-12-15: Architecture re-review (C1, C2, C3) and final polish:
  - C1: Added WebSocket-to-session binding via lws callback handler
  - C2: Added worker pool entry point for WebSocket (ws_work_item_t, worker_pool_assign_websocket)
  - C3: Added cross-thread response queue with lws_cancel_service() wakeup mechanism
  - Added session token specification (128-bit random, hex-encoded, hash map lookup)
  - Added explicit audio buffer ownership transfer semantics
  - Fixed PCM buffer size calculation (64KB, not 32KB)
  - Added webui.max_history_entries config option (prevents unbounded growth)
  - Added conditional compilation for opus_ctx (#ifdef DAWN_WEBUI)
  - Added Opus decode error handling policy with PLC and state reset
  - Added ScriptProcessor deprecation note (future migration to AudioWorkletNode)
- 2025-12-16: **Phase 1-3 Implementation Complete**
  - Changed default port from 8080 to 3000 ("I love you 3000")
  - Implemented token-based session persistence (not IP-based)
  - Text processing uses detached pthread instead of worker pool (simpler for text-only MVP)
  - Added `session_get_for_reconnect()` to allow reconnecting to disconnected sessions
  - Sessions not destroyed on WebSocket close (allows reconnection with preserved history)
  - Abandoned sessions cleaned up immediately when client reconnects with existing token
  - Command processing integrated: `<command>` tag parsing, MQTT publish, follow-up LLM calls
  - Added `worker_pool_get_mosq()` getter for WebUI command processing
  - Frontend: dark theme CSS, ring animations, transcript display, connection status
- 2025-12-16: **Debug Mode and History Replay**
  - Added debug mode toggle checkbox in header
  - Debug entries color-coded: cyan for commands, green for tool results, purple for other debug
  - Mixed text+command messages split: user-facing text shown normally, commands in debug only
  - Tool results from MQTT callbacks sent to WebUI for debug display
  - Conversation history replayed to browser on session reconnect (page refresh preserves transcript)
  - `send_history_impl()` iterates session history and sends user/assistant messages (skips system)
  - FlareSolverr fallback improved: triggers on empty content extraction, not just HTTP 403
- 2025-12-16: **Bug Fixes**
  - Fixed session_destroy blocking on reconnect: call `session_release()` before `session_destroy()`
    to decrement ref_count from 1 to 0 (session was created with ref_count=1, nothing released it)
  - Fixed tool result display artifact: messages starting with `[Tool Result:` now treated as
    complete tool results without regex parsing (avoids early match on embedded `]` characters
    in content like `[bing news]`)
- 2025-12-16: **URL Fetcher Fixes**
  - Fixed truncation handling: set `res = CURLE_OK` so post-loop error check doesn't fail
  - Fixed HTTP code retrieval for truncated responses: call `curl_easy_getinfo()` before breaking
  - Added FlareSolverr fallback for redirect loops (`CURLE_TOO_MANY_REDIRECTS`): JS-based paywalls
    that create infinite redirects between article and auth endpoint now handled via FlareSolverr
- 2025-12-16: **Code Refactoring & Improvements**
  - Extracted `try_flaresolverr_fallback()` helper function in url_fetcher.c (~120 lines reduced)
  - Added retry count to truncation log message for better debugging
  - Weather service: Added US state abbreviation expansion (e.g., "GA" → "Georgia") for geocoding
  - Session timeout: Changed default from 5 minutes to 30 minutes
