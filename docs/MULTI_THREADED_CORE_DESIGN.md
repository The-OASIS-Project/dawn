# DAWN Multi-Threaded Core Design

## Overview

This document describes the architectural changes needed to transform DAWN from a single-threaded, blocking architecture to a multi-threaded design supporting concurrent clients.

**Document Status**: Phases 1-4 implemented. Phase 5 (DAP2) or Phase 6 (WebUI) is NEXT.

**Related Documents:**
- [WEBUI_DESIGN.md](WEBUI_DESIGN.md) - WebUI implementation (requires Phase 4 first)
- [DAP2_DESIGN.md](DAP2_DESIGN.md) - DAP2 protocol specification

---

## Current Architecture (Single-Threaded)

```
┌─────────────────────────────────────────────────────────┐
│                    Main Thread                           │
│                                                          │
│  while (running) {                                       │
│      // Audio capture (blocking ALSA read)               │
│      // VAD processing                                   │
│      // State machine (SILENCE → LISTENING → PROCESSING) │
│      // ASR (Vosk)                                       │
│      // LLM call (BLOCKING - 2-10 seconds)              │
│      // TTS playback                                     │
│      // Network client handling (BLOCKING)               │
│  }                                                       │
└─────────────────────────────────────────────────────────┘
```

### Problems

1. **Network blocks voice** - While processing a DAP client, local mic is unresponsive
2. **Single client limit** - Can only handle one network client at a time
3. **No concurrent sessions** - All clients share single conversation context
4. **LLM latency blocks everything** - 2-10 second API calls freeze the system

### Existing Threading (Foundation to Build On)

DAWN already has working threading patterns:

| Component | Location | Pattern |
|-----------|----------|---------|
| **Audio Capture** | `audio_capture_thread.c` | Dedicated thread, ring buffer |
| **TTS Engine** | `text_to_speech.cpp:412-509` | Producer-consumer queue, condition variables |
| **LLM Processing** | `dawn.c:3396-3420` | Spawned thread per request |
| **Input Queue** | `input_queue.c` | Mutex-protected circular buffer |

**Key Insight**: The TTS implementation demonstrates mature threading patterns we should replicate.

---

## Proposed Architecture: Per-Client Pipelines

**Design Philosophy**: Each client gets its own complete processing pipeline. No artificial bottlenecks. Parallelism scales with hardware.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              DAWN Process                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ╔═══════════════════════════════════════════════════════════════════════╗  │
│  ║                    LOCAL PIPELINE (Main Thread)                       ║  │
│  ╠═══════════════════════════════════════════════════════════════════════╣  │
│  ║  Audio Capture → AEC → VAD → State Machine → ASR                      ║  │
│  ║                                    │                                  ║  │
│  ║                                    ▼                                  ║  │
│  ║                          ┌─────────────────┐                          ║  │
│  ║                          │  LLM Thread     │  (spawned per request)   ║  │
│  ║                          │  - API call     │                          ║  │
│  ║                          │  - Streaming    │                          ║  │
│  ║                          └────────┬────────┘                          ║  │
│  ║                                   ▼                                   ║  │
│  ║                          Local TTS Playback                           ║  │
│  ╚═══════════════════════════════════════════════════════════════════════╝  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                      Network Accept Thread                            │  │
│  │  - Uses select() with 60s timeout (allows periodic cleanup)           │  │
│  │  - Accepts connections, detects DAP1 vs DAP2                          │  │
│  │  - Creates session + assigns to worker (or sends NACK if busy)        │  │
│  │  - Calls session_cleanup_expired() on timeout                         │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                           │                                                 │
│           ┌───────────────┼───────────────┬───────────────┐                 │
│           ▼               ▼               ▼               ▼                 │
│  ╔════════════════╗ ╔════════════════╗ ╔════════════════╗ ╔════════════════╗│
│  ║ WORKER 1       ║ ║ WORKER 2       ║ ║ WORKER 3       ║ ║ WORKER 4       ║│
│  ║ PIPELINE       ║ ║ PIPELINE       ║ ║ PIPELINE       ║ ║ PIPELINE       ║│
│  ╠════════════════╣ ╠════════════════╣ ╠════════════════╣ ╠════════════════╣│
│  ║ ┌────────────┐ ║ ║ ┌────────────┐ ║ ║ ┌────────────┐ ║ ║ ┌────────────┐ ║│
│  ║ │ Session    │ ║ ║ │ Session    │ ║ ║ │ Session    │ ║ ║ │ Session    │ ║│
│  ║ │ + History  │ ║ ║ │ + History  │ ║ ║ │ + History  │ ║ ║ │ + History  │ ║│
│  ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║│
│  ║       ▼        ║ ║       ▼        ║ ║       ▼        ║ ║       ▼        ║│
│  ║ ┌────────────┐ ║ ║ ┌────────────┐ ║ ║ ┌────────────┐ ║ ║ ┌────────────┐ ║│
│  ║ │ ASR        │ ║ ║ │ ASR        │ ║ ║ │ ASR        │ ║ ║ │ ASR        │ ║│
│  ║ │ Context    │ ║ ║ │ Context    │ ║ ║ │ Context    │ ║ ║ │ Context    │ ║│
│  ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║│
│  ║       ▼        ║ ║       ▼        ║ ║       ▼        ║ ║       ▼        ║│
│  ║ ┌────────────┐ ║ ║ ┌────────────┐ ║ ║ ┌────────────┐ ║ ║ ┌────────────┐ ║│
│  ║ │ LLM Call   │ ║ ║ │ LLM Call   │ ║ ║ │ LLM Call   │ ║ ║ │ LLM Call   │ ║│
│  ║ │ (parallel) │ ║ ║ │ (parallel) │ ║ ║ │ (parallel) │ ║ ║ │ (parallel) │ ║│
│  ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║ ║ └─────┬──────┘ ║│
│  ║       ▼        ║ ║       ▼        ║ ║       ▼        ║ ║       ▼        ║│
│  ║ ┌────────────┐ ║ ║ ┌────────────┐ ║ ║ ┌────────────┐ ║ ║ ┌────────────┐ ║│
│  ║ │ TTS Gen    │ ║ ║ │ TTS Gen    │ ║ ║ │ TTS Gen    │ ║ ║ │ TTS Gen    │ ║│
│  ║ │ → Network  │ ║ ║ │ → Network  │ ║ ║ │ → Network  │ ║ ║ │ → Network  │ ║│
│  ║ └────────────┘ ║ ║ └────────────┘ ║ ║ └────────────┘ ║ ║ └────────────┘ ║│
│  ╚════════════════╝ ╚════════════════╝ ╚════════════════╝ ╚════════════════╝│
│                                                                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                           Shared Resources                                  │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐              │
│  │  ASR Model      │  │  TTS Engine     │  │  Command Exec   │              │
│  │  (read-only)    │  │  (mutex)        │  │  (MQTT mutex)   │              │
│  └─────────────────┘  └─────────────────┘  └─────────────────┘              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

1. **No Central Request Queue for Network Clients**
   - Each worker handles full pipeline: Audio → ASR → LLM → TTS → Response
   - Eliminates bottleneck, enables true parallel processing
   - LLM server handles concurrent requests natively

2. **Per-Worker Resources**
   - Own ASR context (model is shared read-only, see `asr_interface.h`)
   - Own session with conversation history
   - Own LLM connection (stateless HTTP)

3. **Shared Resources (Minimal)**
   - ASR model: Read-only, safe for concurrent access (Vosk model or Whisper weights)
   - TTS engine: Mutex-protected, generates WAV on demand
   - MQTT client: Mutex-protected for command publishing

4. **ASR Model Sharing Optimization** (future)
   - Vosk: Could load model once, create recognizers per-worker from shared model
   - Whisper: Model embedded in context (~142MB per context, acceptable for 4 workers)
   - Current implementation loads per-context for simplicity

5. **Local Pipeline Remains Special**
   - Main thread handles audio capture, AEC, VAD
   - Spawns LLM thread per request (existing pattern)
   - Uses local TTS playback (not network)

---

## Key Components

### 1. Session Manager

Manages per-client conversation context with reference counting.

```c
// include/core/session_manager.h

#define MAX_SESSIONS 8
#define SESSION_TIMEOUT_SEC 1800  // 30 minute idle timeout
#define LOCAL_SESSION_ID 0       // Reserved for local microphone

typedef enum {
    SESSION_TYPE_LOCAL,      // Local microphone
    SESSION_TYPE_DAP,        // ESP32 satellite (DAP protocol)
    SESSION_TYPE_DAP2,       // DAP 2.0 satellite (Tier 1 or Tier 2)
    SESSION_TYPE_WEBSOCKET,  // WebUI client
} session_type_t;

/**
 * @brief DAP2 satellite tier (see DAP2_DESIGN.md Section 4)
 */
typedef enum {
    DAP2_TIER_1 = 1,         // Full satellite (RPi) - sends TEXT, receives TEXT
    DAP2_TIER_2 = 2,         // Audio satellite (ESP32) - sends ADPCM, receives ADPCM
} dap2_tier_t;

/**
 * @brief DAP2 satellite identity (from REGISTER message)
 */
typedef struct {
    char uuid[37];           // UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
    char name[64];           // Human-readable name (e.g., "Kitchen Assistant")
    char location[32];       // Room/area (e.g., "kitchen") - used for context
    char hardware_id[64];    // Optional hardware serial
} dap2_identity_t;

/**
 * @brief Session structure
 * @ownership Session manager owns all sessions
 * @thread_safety Protected by session_manager_rwlock
 */
typedef struct {
    uint32_t session_id;
    session_type_t type;
    time_t created_at;
    time_t last_activity;

    // Conversation history (owned by session, protected by history_mutex)
    struct json_object *conversation_history;
    pthread_mutex_t history_mutex;

    // Client-specific data
    int client_fd;           // Socket for network clients (-1 for local)
    pthread_mutex_t fd_mutex;  // Protects client_fd during reconnection
    void *client_data;       // Type-specific data (WebSocket state, etc.)
    char client_ip[INET_ADDRSTRLEN];  // DAP1: IP address for session persistence

    // DAP2-specific fields (only valid when type == SESSION_TYPE_DAP2)
    dap2_tier_t tier;                  // Tier 1 (text) or Tier 2 (audio)
    dap2_identity_t identity;          // UUID, name, location
    struct {
        bool local_asr;                // Satellite can transcribe locally
        bool local_tts;                // Satellite can synthesize locally
        bool wake_word;                // Satellite has wake word detection
    } capabilities;

    // Cancellation
    volatile bool disconnected;  // Set on client disconnect

    // Reference counting for safe access (two-phase destruction pattern)
    int ref_count;
    pthread_mutex_t ref_mutex;
    pthread_cond_t ref_zero_cond;  // Signaled when ref_count reaches 0
} session_t;

// Lifecycle
int session_manager_init(void);
void session_manager_cleanup(void);

/**
 * @brief Create new session
 * @locks session_manager_rwlock (write)
 * @lock_order 1
 */
session_t *session_create(session_type_t type, int client_fd);

/**
 * @brief Create new DAP2 session with identity (from REGISTER message)
 * @param client_fd Client socket
 * @param tier DAP2_TIER_1 (text) or DAP2_TIER_2 (audio)
 * @param identity Satellite identity (UUID, name, location)
 * @param local_asr Satellite can transcribe locally
 * @param local_tts Satellite can synthesize locally
 * @param wake_word Satellite has wake word detection
 * @return New session, or existing session if UUID matches (reconnection)
 * @locks session_manager_rwlock (write) for entire operation (atomic check-and-create)
 * @note If UUID matches an existing session:
 *       1. Acquires session->fd_mutex
 *       2. Updates client_fd (old socket is invalid after reconnect)
 *       3. Clears disconnected flag
 *       4. Releases fd_mutex
 *       5. Returns existing session with preserved conversation history
 */
session_t *session_create_dap2(int client_fd, dap2_tier_t tier,
                               const dap2_identity_t *identity,
                               bool local_asr, bool local_tts, bool wake_word);

/**
 * @brief Get or create DAP1 session by client IP (legacy protocol)
 *
 * DAP1 clients don't send a REGISTER message with UUID, so we use IP address
 * as a simple session identifier. This allows conversation history to persist
 * across reconnections from the same IP address.
 *
 * @param client_fd Client socket
 * @param client_ip Client IP address string
 * @return Existing session if IP matches, or new session if not found
 * @locks session_manager_rwlock (write) for entire operation
 * @note Caller MUST call session_release() when done
 * @warning This is a temporary solution for DAP1 testing. DAP2 uses proper UUIDs.
 */
session_t *session_get_or_create_dap(int client_fd, const char *client_ip);

/**
 * @brief Get session by ID (increments ref_count while holding rwlock)
 * @locks session_manager_rwlock (read), then session->ref_mutex (brief)
 * @lock_order Acquires rwlock, increments ref, releases rwlock BEFORE caller uses session
 * @note Caller MUST call session_release() when done
 * @note Returns NULL for disconnected sessions (prevents new refs to dying sessions)
 */
session_t *session_get(uint32_t session_id);

/**
 * @brief Release session reference (decrements ref_count)
 * @note Signals ref_zero_cond when ref_count reaches 0
 */
void session_release(session_t *session);

/**
 * @brief Get local session (always exists)
 */
session_t *session_get_local(void);

/**
 * @brief Mark session as disconnected and destroy when ref_count=0
 * @note Two-phase destruction:
 *       1. Mark disconnected + remove from active list (prevents new refs)
 *       2. Wait for ref_count=0 via ref_zero_cond, then free
 */
void session_destroy(uint32_t session_id);

/**
 * @brief Add message to session's conversation history
 * @locks session->history_mutex
 * @lock_order 3
 */
void session_add_message(session_t *session, const char *role, const char *content);

/**
 * @brief Call LLM with session's conversation history
 * @param session Session with conversation context
 * @param user_text User's query text
 * @return LLM response (caller must free), or NULL on error/cancel
 * @note Adds user message before call, assistant response after call
 * @note Returns NULL if session->disconnected is set (cancel)
 * @note Prepends location context if session->identity.location is set
 */
char *session_llm_call(session_t *session, const char *user_text);

/**
 * @brief Cleanup expired sessions (called periodically)
 */
void session_cleanup_expired(void);
```

### 2. TTS Generator (Thread-Safe)

Workers call TTS to generate WAV data for network responses. The existing TTS engine is already mutex-protected.

```c
// Already exists in text_to_speech.h:

/**
 * @brief Generate WAV audio data from text for network transmission
 * @param text The text to be converted to WAV audio
 * @param wav_data_out Pointer to receive allocated WAV data (caller must free)
 * @param wav_size_out Pointer to receive WAV data size in bytes
 * @return 0 on success, -1 on error
 * @thread_safety Uses existing tts_mutex
 */
int text_to_speech_to_wav(const char *text, uint8_t **wav_data_out, size_t *wav_size_out);
```

**Note**: This function already exists - no implementation needed.

### 3. Worker Thread Pool (Full Pipeline)

Each worker handles a complete client pipeline: Audio → ASR → LLM → TTS → Response.

```c
// include/core/worker_pool.h

#include "asr/asr_interface.h"  // For asr_context_t, asr_engine_type_t

#define WORKER_POOL_SIZE 4
#define WORKER_LLM_TIMEOUT_MS 30000  // 30 second LLM timeout

/**
 * @brief Per-worker context - everything needed for full pipeline
 */
typedef struct {
    int worker_id;
    pthread_t thread;

    // Client connection (assigned per-request)
    int client_fd;
    session_t *session;          // Session with conversation history

    // Per-worker resources (created at init, reused)
    asr_context_t *asr_ctx;      // Own ASR context (Vosk or Whisper)
    adpcm_state_t *adpcm_state;  // Own ADPCM codec state (for Tier 2)

    // State
    volatile bool busy;          // Currently handling a client
    volatile bool shutdown;      // Shutdown requested
} worker_context_t;

/**
 * @brief Initialize worker pool (EAGER initialization)
 * @param engine_type ASR engine to use (ASR_ENGINE_VOSK or ASR_ENGINE_WHISPER)
 * @param model_path Path to ASR model (Vosk model dir or Whisper .bin file)
 * @return SUCCESS (0) or FAILURE (1)
 *
 * @note EAGER INITIALIZATION: All worker resources allocated at startup:
 *       - WORKER_POOL_SIZE ASR contexts created immediately
 *       - WORKER_POOL_SIZE ADPCM codec states created immediately
 *       - Worker threads spawned and waiting for clients
 *       - Fail fast if model load fails (don't wait for first client)
 *       - Memory usage is predictable and known at startup
 *
 * @note Rationale: Lazy init would cause 1-2s latency on first client
 *       (especially Whisper model load). For embedded system, eager init
 *       ensures predictable behavior and simpler error handling.
 */
int worker_pool_init(asr_engine_type_t engine_type, const char *model_path);

/**
 * @brief Shutdown worker pool gracefully
 * @note Sets session->disconnected on all active sessions first (aborts LLM calls)
 * @note Waits up to 35s for workers to finish (> 30s LLM timeout)
 * @note Uses pthread_cancel() as last resort, then force-closes sockets
 */
void worker_pool_shutdown(void);

/**
 * @brief Assign client to available worker
 * @param client_fd Client socket
 * @param session Client session (created by accept thread)
 * @return SUCCESS or FAILURE if all workers busy
 */
int worker_pool_assign_client(int client_fd, session_t *session);

/**
 * @brief Get worker utilization for metrics
 * @return Number of active workers (0 to WORKER_POOL_SIZE)
 */
int worker_pool_active_count(void);
```

---

## Thread Synchronization

### Lock Analysis: Exclusive Ownership Simplifies Everything

**Key insight**: In this per-client pipeline design, **each worker owns its session exclusively** after the accept thread assigns it. This dramatically simplifies locking.

**During normal worker operation** (processing a request):
- Worker has exclusive access to its session
- No other thread will touch the session's history, fd, or data
- Per-session locks (`fd_mutex`, `history_mutex`) are **NOT needed** for normal operation

**Per-session locks are ONLY needed for**:
1. **Reconnection** - If a satellite reconnects (same UUID), the accept thread may update `client_fd` on an existing session. But this only happens when:
   - The old worker has finished (session idle), OR
   - The old connection is dead (worker detected disconnect)

   In both cases, there's no concurrent access. The atomic `session_create_dap2()` handles this safely.

2. **Session destruction** - The `ref_mutex` + `ref_zero_cond` pattern ensures cleanup waits for the worker to finish.

### Minimal Lock Model

```
┌─────────────────────────────────────────────────────┐
│ Locks Actually Required                              │
├─────────────────────────────────────────────────────┤
│ LIFECYCLE LOCKS (accept thread / cleanup thread)    │
│ ├── session_manager_rwlock: Create/destroy sessions │
│ └── ref_mutex + ref_zero_cond: Safe destruction     │
├─────────────────────────────────────────────────────┤
│ SHARED RESOURCE LOCKS (all workers)                 │
│ ├── tts_mutex: Single TTS engine, workers queue     │
│ └── mqtt_mutex: Single MQTT client for commands     │
└─────────────────────────────────────────────────────┘
```

### What About fd_mutex and history_mutex?

**Keep them in the struct** but understand when they're actually needed:

| Lock | When Needed | Normal Worker Operation |
|------|-------------|------------------------|
| `fd_mutex` | Reconnection edge case | NOT needed - worker owns socket |
| `history_mutex` | Future: streaming LLM tokens | NOT needed - worker owns history |
| `tts_mutex` | Always | YES - shared resource |
| `mqtt_mutex` | Always | YES - shared resource |

**Implementation guidance**: The worker code shown earlier uses `fd_mutex` defensively. This is fine - it's a no-op when there's no contention (single owner). But it's not strictly required for correctness given exclusive ownership.

### Lock Order (for the cases where multiple locks ARE held)

If future features require holding multiple locks (e.g., streaming LLM tokens to socket while updating history), follow this order:

```
1. session_manager_rwlock (outermost, brief hold)
2. ref_mutex (brief hold for ref counting)
3. history_mutex (if needed)
4. fd_mutex (if needed)
5. tts_mutex (leaf)
6. mqtt_mutex (leaf)
```

**RULE**: If you find yourself holding multiple per-session locks simultaneously, reconsider the design - exclusive ownership should make this unnecessary.

### Mutexes Summary

| Resource | Lock Type | Contention | Notes |
|----------|-----------|------------|-------|
| Session Manager | `pthread_rwlock` | Low | Only on connect/disconnect, released quickly |
| Session Ref Count | `pthread_mutex` (per-session) | Low | Brief hold during inc/dec only |
| Conversation History | `pthread_mutex` (per-session) | None | Each worker owns its session |
| TTS Engine | `pthread_mutex` (existing) | Low | Brief hold during WAV generation |
| MQTT Client | `pthread_mutex` | Low | Leaf lock |
| Metrics | `atomic ops` | Medium | No locking needed |

**Key Insight**: Per-client pipelines dramatically reduce lock contention. Workers rarely need to synchronize.

---

## State Machine Changes

### Current Issues

1. `DAWN_STATE_NETWORK_PROCESSING` creates coupling between network and local state
2. Main loop waits during LLM processing

### New Design

**Remove** `DAWN_STATE_NETWORK_PROCESSING` from state machine. Network clients operate completely independently via worker threads - they don't touch the state machine at all.

**Local Pipeline** (unchanged from current, keeps working pattern):
```
SILENCE → WAKEWORD_LISTEN → COMMAND_RECORDING → PROCESS_COMMAND → SILENCE
                                                      │
                                            (spawns LLM thread)
```

- Existing LLM thread pattern (`dawn.c:3396-3420`) continues to work
- LLM thread handles streaming response and TTS playback
- Main thread returns to SILENCE after spawning LLM thread

**Network Pipelines** (completely separate):
```
Worker Thread:  Receive Audio → ASR → LLM → TTS → Send Response
                     │                              │
                     └──────── (no state machine) ──┘
```

- Workers don't interact with dawn state machine
- Each worker handles full pipeline for its client
- Multiple workers run in parallel

---

## Memory Ownership Transfer

### Clear Ownership Rules

| Resource | Owner | Notes |
|----------|-------|-------|
| Session | Session Manager | Created on connect, destroyed on disconnect/timeout |
| Conversation History | Session | JSON object owned by session, protected by history_mutex |
| Audio Data (network) | Worker | Received from socket, freed after ASR |
| WAV Response | Worker | Generated by TTS, sent to client, freed by worker |
| ASR Context | Worker | Created at init via `asr_init()`, reused across clients |

### Worker Pipeline Ownership (Simple)

> **Note**: The following is simplified pseudocode showing the basic pipeline flow.
> The actual implementation (`worker_thread_dap2()`, see next section) includes
> DAP2 protocol handling and tier-based branching.

```c
// Simplified pseudocode - see worker_thread_dap2() for actual implementation
void *worker_thread(void *arg) {
    worker_context_t *ctx = (worker_context_t *)arg;

    while (!ctx->shutdown) {
        // Wait for client assignment
        wait_for_client(ctx);

        // Receive audio (worker allocates)
        void *audio = receive_audio(ctx->client_fd);

        // ASR (uses worker's ASR context - Vosk or Whisper)
        asr_result_t *result = asr_process_partial(ctx->asr_ctx, audio, num_samples);
        asr_finalize(ctx->asr_ctx);
        char *text = strdup(result->text);
        asr_result_free(result);
        free(audio);  // Done with audio

        // LLM (uses session's history)
        char *response = llm_call(ctx->session, text);
        free(text);  // Done with text

        // TTS (generates WAV, worker owns it)
        uint8_t *wav;
        size_t wav_size;
        text_to_speech_to_wav(response, &wav, &wav_size);
        free(response);  // Done with response

        // Send to client
        send_response(ctx->client_fd, wav, wav_size);
        free(wav);  // Done with WAV

        // Mark worker available
        ctx->busy = false;
    }
}
```

**Key**: Each step consumes input and produces output. No shared buffers between workers.

### DAP2 Tier-Based Pipeline Branching

DAP2 satellites have different capabilities, so the worker pipeline branches based on tier:

```c
void *worker_thread_dap2(void *arg) {
    worker_context_t *ctx = (worker_context_t *)arg;

    while (!ctx->shutdown) {
        wait_for_client(ctx);

        if (ctx->session->type != SESSION_TYPE_DAP2) {
            // Handle legacy DAP1 or WebSocket (full pipeline)
            handle_legacy_client(ctx);
            continue;
        }

        // Read message type (with fd_mutex for reconnection safety)
        pthread_mutex_lock(&ctx->session->fd_mutex);
        int msg_type = dap2_read_message_type(ctx->session->client_fd);
        pthread_mutex_unlock(&ctx->session->fd_mutex);

        // Validate message type matches tier (#4: tier validation)
        if (ctx->session->tier == DAP2_TIER_1 && msg_type != DAP2_MSG_QUERY) {
            LOG_ERROR("Tier 1 sent non-text message 0x%02x, expected QUERY", msg_type);
            send_nack(ctx->session, DAP2_ERR_INVALID_MSG_TYPE);
            continue;
        }
        if (ctx->session->tier == DAP2_TIER_2 && msg_type != DAP2_MSG_QUERY_AUDIO) {
            LOG_ERROR("Tier 2 sent non-audio message 0x%02x, expected QUERY_AUDIO", msg_type);
            send_nack(ctx->session, DAP2_ERR_INVALID_MSG_TYPE);
            continue;
        }

        // DAP2: Branch based on tier
        switch (ctx->session->tier) {
        case DAP2_TIER_1:
            handle_tier1_text_query(ctx);
            break;

        case DAP2_TIER_2:
            handle_tier2_audio_query(ctx);
            break;
        }

        ctx->busy = false;
    }
}

/**
 * @brief Handle Tier 1 (text-only) query
 * Pipeline: Receive TEXT → LLM → Send TEXT (streaming)
 */
void handle_tier1_text_query(worker_context_t *ctx) {
    // Receive text query (QUERY message per DAP2_DESIGN.md Section 5.2)
    char *query_text = dap2_recv_query_text(ctx->session->client_fd);
    if (!query_text) return;

    // LLM call with session context (adds to conversation history)
    char *response = session_llm_call(ctx->session, query_text);
    free(query_text);
    if (!response) return;

    // Stream response - send full text, satellite splits sentences locally
    // (Simpler than server-side splitting; Tier 1 has compute for this)
    dap2_send_response_text(ctx->session->client_fd, response);
    dap2_send_packet(ctx->session->client_fd, DAP2_MSG_RESPONSE_END, NULL, 0);
    free(response);
}

/**
 * @brief Handle Tier 2 (audio) query
 * Pipeline: Receive ADPCM → Decode → ASR → LLM → TTS (PCM) → Encode ADPCM → Send
 */
void handle_tier2_audio_query(worker_context_t *ctx) {
    // Reset ADPCM state for new audio stream
    adpcm_state_reset(ctx->adpcm_state);

    // Receive ADPCM audio (QUERY_AUDIO message)
    uint8_t *adpcm_in;
    size_t adpcm_size;
    if (dap2_recv_query_audio(ctx->session->client_fd, &adpcm_in, &adpcm_size) != 0) {
        return;
    }

    // Decode ADPCM → PCM (uses worker's codec state)
    int16_t *pcm_audio;
    size_t pcm_samples;
    adpcm_decode(ctx->adpcm_state, adpcm_in, adpcm_size, &pcm_audio, &pcm_samples);
    free(adpcm_in);

    // ASR (uses worker's ASR context)
    asr_result_t *result = asr_process_chunk(ctx->asr_ctx, pcm_audio, pcm_samples);
    asr_finalize(ctx->asr_ctx);
    char *text = strdup(result->text);
    asr_result_free(result);
    free(pcm_audio);

    // LLM call with session context
    char *response = session_llm_call(ctx->session, text);
    free(text);
    if (!response) return;

    // TTS → PCM directly (avoids WAV header overhead)
    // NOTE: text_to_speech_to_pcm() implemented in Phase 4
    int16_t *tts_pcm;
    size_t tts_samples;
    text_to_speech_to_pcm(response, &tts_pcm, &tts_samples);
    free(response);

    // Encode PCM → ADPCM (uses worker's codec state, reset for encode)
    adpcm_state_reset(ctx->adpcm_state);
    uint8_t *adpcm_out;
    size_t adpcm_out_size;
    adpcm_encode(ctx->adpcm_state, tts_pcm, tts_samples, &adpcm_out, &adpcm_out_size);
    free(tts_pcm);

    // Send audio response
    dap2_send_response_audio(ctx->session->client_fd, adpcm_out, adpcm_out_size);
    dap2_send_packet(ctx->session->client_fd, DAP2_MSG_RESPONSE_END, NULL, 0);
    free(adpcm_out);
}
```

**Key differences by tier**:

| Step | Tier 1 (RPi) | Tier 2 (ESP32) |
|------|--------------|----------------|
| Input | Text (QUERY) | ADPCM audio (QUERY_AUDIO) |
| ASR | Skipped (done locally) | Worker performs ASR |
| LLM | Worker calls LLM | Worker calls LLM |
| TTS | Skipped (done locally) | Worker generates TTS |
| Output | Text (RESPONSE_STREAM) | ADPCM audio (RESPONSE_AUDIO) |
| Latency | Very low (~100-500 bytes) | Higher (~30-50KB audio) |

---

## Graceful Shutdown Sequence

```
┌─────────────────────────────────────────────────────────────────┐
│                    Shutdown Sequence                             │
├─────────────────────────────────────────────────────────────────┤
│ 1. Signal handler sets quit flag                                 │
│    └── All threads check get_quit() in their loops              │
├─────────────────────────────────────────────────────────────────┤
│ 2. Stop accepting new connections                                │
│    ├── Accept thread stops listen loop                           │
│    └── Close listen socket                                       │
├─────────────────────────────────────────────────────────────────┤
│ 3. Main thread stops audio capture                               │
│    └── Exits main loop, waits for LLM thread if active          │
├─────────────────────────────────────────────────────────────────┤
│ 4. Worker threads finish current clients                         │
│    ├── Mark ALL active sessions as disconnected                  │
│    │   └── This aborts in-flight LLM calls via cancel_flag       │
│    ├── Set shutdown flag on all workers                          │
│    ├── pthread_join with 35s timeout (> 30s LLM timeout)         │
│    ├── pthread_cancel() as last resort if still blocked          │
│    └── Force close sockets if timeout                            │
├─────────────────────────────────────────────────────────────────┤
│ 5. Cleanup resources                                             │
│    ├── Destroy all sessions (frees conversation histories)       │
│    ├── Destroy ASR contexts (asr_cleanup() for each worker)      │
│    ├── Cleanup TTS engine                                        │
│    └── Close any remaining sockets                               │
└─────────────────────────────────────────────────────────────────┘
```

**Note**: Simpler than queued design - no request queue to drain.

---

## Implementation Plan (Revised for Per-Client Pipelines)

### Phase 1: Session Manager + TTS Generate

**Goal**: Foundation for multi-client - sessions and WAV generation.

1. Implement `src/core/session_manager.c` with rwlock and two-phase destruction
2. Create "local" session at startup (session_id = 0)
3. Verify `text_to_speech_to_wav()` works correctly (already implemented)
4. Verify ASR multi-context support (test before Phase 2)

**Test**: Session create/destroy works, WAV generation produces valid audio

**Files**:
```
include/core/session_manager.h
src/core/session_manager.c
```

**ASR multi-context verification test** (must pass before Phase 2):
```c
// Test: Create 2 ASR contexts in 2 threads (per asr_interface.h docs)
// asr_context_t is NOT thread-safe, so each thread needs its own
asr_context_t *ctx1 = asr_init(ASR_ENGINE_VOSK, model_path, 16000);
asr_context_t *ctx2 = asr_init(ASR_ENGINE_VOSK, model_path, 16000);
// Verify both work concurrently in parallel threads
// Note: Whisper can also be tested with ASR_ENGINE_WHISPER
```

### Phase 2: Worker Pool + Accept Thread

**Goal**: Full per-client pipelines for network clients.

1. Create `src/core/worker_pool.c` with full pipeline support
2. Create `src/network/accept_thread.c` (uses select() with 60s timeout)
3. Each worker: own ASR context (via `asr_init()`), handles Audio → ASR → LLM → TTS → Response
4. Accept thread creates session, assigns to worker (sends NACK if all busy)
5. **Remove** `DAWN_STATE_NETWORK_PROCESSING` from state machine:
   - Remove from `include/state_machine.h` enum
   - Remove from `dawn_state_name()` switch
   - Update `dawn.c` main loop to remove network processing block

**Test**: Multiple DAP clients can connect and converse simultaneously

**Files**:
```
include/core/worker_pool.h
include/network/accept_thread.h
src/core/worker_pool.c
src/network/accept_thread.c
include/state_machine.h  # Remove DAWN_STATE_NETWORK_PROCESSING
```

### Phase 3: Per-Session Conversation History

**Goal**: Each client has isolated conversation context.

1. Move `conversation_history` from global to per-session
2. Add `session->history_mutex` for thread safety
3. Update LLM interface to use session context
4. Local pipeline uses local session (session_id = 0)
5. **DAP1 Session Persistence**: Implement IP-based session lookup for legacy clients

**Test**: Concurrent clients have independent conversations

#### DAP1 IP-Based Session Persistence

DAP1 clients (ESP32 satellites using the legacy protocol) don't send a REGISTER message
with a UUID like DAP2 clients do. To enable testing of per-session conversation history
with DAP1 clients, we use the client's IP address as a simple session identifier.

**Implementation**:
```c
// Session struct includes client_ip field
char client_ip[INET_ADDRSTRLEN];  // DAP1: IP address for session persistence

// Accept thread uses session_get_or_create_dap() instead of session_create()
session_t *session = session_get_or_create_dap(client_fd, client_ip);
```

**Behavior**:
- First connection from IP: Creates new session, assigns to worker
- Reconnection from same IP: Returns existing session with preserved history
- Session timeout (30 min idle): Session destroyed, next connection creates fresh session

**Limitations** (acceptable for testing):
- Multiple devices behind NAT appear as single session
- IP changes (DHCP renewal) create new session
- No explicit identity or location context (unlike DAP2)

**Note**: This is a temporary solution for DAP1 testing. DAP2 (Phase 5) uses proper
UUID-based identity from REGISTER messages, enabling robust reconnection and per-device
context (name, location, capabilities).

#### Remote vs Local System Prompts

Network satellite clients (DAP/DAP2) receive a **different system prompt** than the local
microphone interface. This allows local-only commands (HUD, helmet) to be restricted to
the primary interface while general commands (date, time, etc.) are available everywhere.

**Topic-Based Filtering**:

The `commands_config_nuevo.json` file defines a `topic` field for each device:
- `"hud"` - HUD-specific commands (armor display, map, detect, info, viewing, etc.)
- `"helmet"` - Helmet-specific commands (faceplate)
- `"dawn"` - General commands (date, time, volume, reset conversation)

**Implementation**:
```c
// llm/llm_command_parser.c
static const char *excluded_remote_topics[] = {"hud", "helmet", NULL};

const char *get_remote_command_prompt(void);  // Excludes HUD/helmet commands
const char *get_local_command_prompt(void);   // All commands (for local interface)

// core/session_manager.c - DAP sessions use remote prompt
session_init_system_prompt(session, get_remote_command_prompt());
```

**Prompt Contents**:

| Interface | System Prompt | Available Commands |
|-----------|---------------|-------------------|
| Local microphone | `get_local_command_prompt()` | All commands (HUD, helmet, general) |
| Network satellite | `get_remote_command_prompt()` | General commands only (date, time, volume, music) |

**Benefits**:
- Remote clients can't accidentally trigger HUD commands meant for the operator
- Simpler, cleaner prompt for satellites (fewer irrelevant options)
- Same AI personality across all interfaces

### Phase 3.5: MQTT Request/Response for Command Results

**Goal**: Enable worker threads to receive command results via MQTT request/response pattern.

#### Problem Statement

Currently, when a worker processes a command like "What time is it?":
1. LLM returns: `"The time is <command>{"device":"time","action":"get"}</command>"`
2. Worker publishes command to MQTT
3. Main thread executes callback → speaks result **locally**
4. Remote client never receives the actual time

Worker threads need to receive command results to include them in TTS for remote clients.

#### Design: MQTT Request/Response Pattern

Add request/response semantics on top of MQTT pub/sub:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        MQTT Request/Response Flow                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Worker Thread                          Main Thread                          │
│  ─────────────                          ───────────                          │
│      │                                      │                                │
│      │ 1. Generate request_id               │                                │
│      │    "worker_2_1702045678"             │                                │
│      │                                      │                                │
│      │ 2. Register in pending_requests      │                                │
│      │    (mutex + cond_var + result_buf)   │                                │
│      │                                      │                                │
│      │ 3. Publish command ──────────────────┼──► MQTT Broker                 │
│      │    {"device":"time",                 │         │                      │
│      │     "action":"get",                  │         │                      │
│      │     "request_id":"worker_2_..."}     │         │                      │
│      │                                      │         ▼                      │
│      │ 4. Wait on cond_var                  │    on_message()                │
│      │    (with 5s timeout)                 │         │                      │
│      │         │                            │    5. Has request_id?          │
│      │         │                            │         │                      │
│      │         │                            │    6. Execute callback         │
│      │         │                            │       timeCallback()           │
│      │         │                            │       → "3:45 PM"              │
│      │         │                            │         │                      │
│      │         │                            │    7. Route reply to worker    │
│      │         │◄────────────────────────────────── signal cond_var          │
│      │         │                            │                                │
│      │ 8. Receive result                    │                                │
│      │    "3:45 PM"                         │                                │
│      │                                      │                                │
│      │ 9. Substitute into TTS text          │                                │
│      │    "The time is 3:45 PM"             │                                │
│      │                                      │                                │
│      ▼                                      │                                │
│   Continue pipeline                         │                                │
│   (TTS → Send to client)                    │                                │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### 1. Pending Request Registry

```c
// include/core/command_router.h

#define MAX_PENDING_REQUESTS 16
#define COMMAND_RESULT_TIMEOUT_MS 5000

/**
 * @brief Pending request entry for worker waiting on command result
 */
typedef struct {
   char request_id[48];           // "worker_<id>_<timestamp>"
   pthread_mutex_t mutex;
   pthread_cond_t result_ready;
   char *result;                  // Callback result (set by main thread)
   bool completed;                // Result ready flag
   bool timed_out;                // Timeout flag
} pending_request_t;

/**
 * @brief Initialize command router (call at startup)
 */
int command_router_init(void);

/**
 * @brief Shutdown command router
 */
void command_router_shutdown(void);

/**
 * @brief Register a pending request (called by worker before publish)
 * @param worker_id Worker ID for request_id generation
 * @return Allocated pending_request_t, or NULL if registry full
 * @note Caller must call command_router_wait() or command_router_cancel()
 */
pending_request_t *command_router_register(int worker_id);

/**
 * @brief Wait for command result with timeout
 * @param req Pending request from command_router_register()
 * @param timeout_ms Timeout in milliseconds
 * @return Result string (caller must free), or NULL on timeout/error
 * @note Automatically unregisters the request after return
 */
char *command_router_wait(pending_request_t *req, int timeout_ms);

/**
 * @brief Cancel a pending request (called on worker disconnect)
 * @param req Pending request to cancel
 */
void command_router_cancel(pending_request_t *req);

/**
 * @brief Route a command result to waiting worker (called by main thread)
 * @param request_id Request ID from command JSON
 * @param result Result string from callback
 * @return true if request found and signaled, false otherwise
 */
bool command_router_deliver(const char *request_id, const char *result);
```

#### 2. Modified Command Message Format

**Outgoing command (worker → MQTT):**
```json
{
   "device": "time",
   "action": "get",
   "request_id": "worker_2_1702045678901"
}
```

**Reply routing (main thread on_message):**
```c
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
   json_object *parsed = json_tokener_parse(msg->payload);

   // Check if this is a command with request_id
   json_object *request_id_obj;
   if (json_object_object_get_ex(parsed, "request_id", &request_id_obj)) {
      const char *request_id = json_object_get_string(request_id_obj);

      // Execute callback and get result
      char *result = execute_callback_and_get_result(parsed);

      // Route result to waiting worker
      if (result) {
         command_router_deliver(request_id, result);
         free(result);
      }
   } else {
      // Legacy command (no request_id) - execute as before
      parseJsonCommandandExecute(msg->payload);
   }

   json_object_put(parsed);
}
```

#### 3. Worker Command Processing Flow

```c
// In worker_handle_client(), after LLM response:

// Step 5b: Process commands and prepare TTS text
char *tts_text = strdup(llm_response);

// Find and process each command tag
char *cmd_start;
while ((cmd_start = strstr(tts_text, "<command>")) != NULL) {
   char *cmd_end = strstr(cmd_start, "</command>");
   if (!cmd_end) break;

   // Extract command JSON
   size_t json_start = cmd_start + strlen("<command>") - tts_text;
   size_t json_len = cmd_end - (cmd_start + strlen("<command>"));
   char *cmd_json = strndup(tts_text + json_start, json_len);

   // Register for response
   pending_request_t *req = command_router_register(ctx->worker_id);
   if (req) {
      // Add request_id to command JSON
      json_object *cmd = json_tokener_parse(cmd_json);
      json_object_object_add(cmd, "request_id",
                             json_object_new_string(req->request_id));
      const char *cmd_with_id = json_object_to_json_string(cmd);

      // Publish command
      mosquitto_publish(worker_mosq, NULL, APPLICATION_NAME,
                        strlen(cmd_with_id), cmd_with_id, 0, false);

      // Wait for result
      char *result = command_router_wait(req, COMMAND_RESULT_TIMEOUT_MS);

      if (result) {
         // Substitute result into TTS text
         // "The time is <command>...</command>" → "The time is 3:45 PM"
         replace_command_tag(tts_text, cmd_start, cmd_end, result);
         free(result);
      } else {
         // Timeout - remove command tag, log warning
         remove_command_tag(tts_text, cmd_start, cmd_end);
         LOG_WARNING("Worker %d: Command timed out", ctx->worker_id);
      }

      json_object_put(cmd);
   }
   free(cmd_json);
}

// Continue with TTS generation using tts_text with results substituted
```

#### 4. Optional: Multi-Turn LLM Integration

For commands where LLM formatting is desired (e.g., search results), the worker can
optionally send the result back to the LLM:

```c
// Policy decision per-command type
typedef enum {
   CMD_RESULT_DIRECT,      // Substitute result directly into TTS text
   CMD_RESULT_LLM_FORMAT,  // Send result to LLM for natural formatting
} cmd_result_policy_t;

// Configuration (could be per-device in commands_config)
cmd_result_policy_t get_result_policy(const char *device) {
   // Simple commands: direct substitution
   if (strcmp(device, "time") == 0) return CMD_RESULT_DIRECT;
   if (strcmp(device, "date") == 0) return CMD_RESULT_DIRECT;

   // Complex results: LLM formatting
   if (strcmp(device, "search") == 0) return CMD_RESULT_LLM_FORMAT;
   if (strcmp(device, "weather") == 0) return CMD_RESULT_LLM_FORMAT;

   return CMD_RESULT_DIRECT;  // Default
}

// In worker, after receiving result:
if (get_result_policy(device) == CMD_RESULT_LLM_FORMAT) {
   // Send result to LLM for natural formatting
   char prompt[1024];
   snprintf(prompt, sizeof(prompt),
            "[TOOL RESULT] Format this naturally for the user: %s", result);
   char *formatted = session_llm_call(ctx->session, prompt);
   if (formatted) {
      // Use LLM's formatted response for TTS
      replace_tts_text(tts_text, formatted);
      free(formatted);
   }
} else {
   // Direct substitution
   replace_command_tag(tts_text, cmd_start, cmd_end, result);
}
```

#### 5. Future Extension: External Services

This architecture supports external services that publish results via MQTT:

```
┌──────────────┐      MQTT       ┌──────────────┐      MQTT       ┌──────────────┐
│   Worker     │ ──────────────► │   Broker     │ ──────────────► │  Search Svc  │
│   Thread     │                 │              │                 │  (Python)    │
│              │                 │              │                 │              │
│  Waiting...  │ ◄────────────── │              │ ◄────────────── │  Publishes   │
│              │   result+id     │              │   result+id     │  result      │
└──────────────┘                 └──────────────┘                 └──────────────┘
```

External services just need to:
1. Subscribe to `dawn` topic
2. Look for commands they handle (e.g., `"device": "search"`)
3. Execute the operation
4. Publish result with same `request_id`

**Example Python search service:**
```python
def on_message(client, userdata, msg):
    cmd = json.loads(msg.payload)
    if cmd.get("device") == "search" and "request_id" in cmd:
        results = perform_search(cmd["action"], cmd.get("value"))
        client.publish("dawn", json.dumps({
            "request_id": cmd["request_id"],
            "result": results
        }))
```

#### 6. Thread Safety

| Component | Lock | Notes |
|-----------|------|-------|
| Pending Request Registry | `registry_mutex` | Protects request array |
| Individual Request | `req->mutex` | Protects result + completion flag |
| MQTT Publish | `mosquitto_publish()` | Thread-safe (per libmosquitto) |
| Callback Execution | `registry_mutex` held | Existing callbacks use static buffers |

**Lock Order:**
```
registry_mutex → req->mutex (never reverse)
```

**Critical Lock Ordering Rules:**

1. **NEVER acquire `registry_mutex` while holding `req->mutex`** (deadlock risk)

2. **When delivering results** (main thread `command_router_deliver`):
   - Acquire `registry_mutex` first
   - Search for request by ID
   - Acquire `req->mutex` **while still holding** `registry_mutex`
   - Signal condition variable
   - Release in reverse order: `req->mutex` → `registry_mutex`
   - **Rationale**: Prevents slot reuse race between finding and signaling

3. **When waiting for results** (worker `command_router_wait`):
   - Acquire `req->mutex` ONLY (no `registry_mutex`)
   - Wait on condition variable with timeout
   - On return (success or timeout), release `req->mutex`
   - Then acquire `registry_mutex` to unregister slot
   - **Never hold both locks during wait**

4. **When registering** (worker `command_router_register`):
   - Acquire `registry_mutex`
   - Find free slot, initialize
   - Release `registry_mutex`
   - Worker now owns the slot exclusively until `wait()` or `cancel()`

**Corrected `command_router_deliver()` Implementation:**

```c
bool command_router_deliver(const char *request_id, const char *result) {
   pthread_mutex_lock(&registry_mutex);  // Hold throughout delivery

   // Find request by ID (safe: registry_mutex held)
   pending_request_t *req = NULL;
   for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
      if (registry[i].in_use &&
          strcmp(registry[i].request_id, request_id) == 0) {
         req = &registry[i];
         break;
      }
   }

   if (!req) {
      pthread_mutex_unlock(&registry_mutex);
      return false;  // Request not found (timed out or never existed)
   }

   // Acquire req->mutex while still holding registry_mutex
   // This prevents the worker from unregistering between find and signal
   pthread_mutex_lock(&req->mutex);

   if (!req->timed_out) {
      req->result = strdup(result);
      req->completed = true;
      pthread_cond_signal(&req->result_ready);
   }
   // else: Worker already timed out, discard result

   pthread_mutex_unlock(&req->mutex);
   pthread_mutex_unlock(&registry_mutex);

   return true;
}
```

**Static Callback Buffer Note:**

Existing callbacks (e.g., `timeCallback()`) use static return buffers. This is safe because:
- All MQTT message processing happens in the main thread's `on_message()` callback
- Only one callback executes at a time
- The result is `strdup()`'d before releasing any locks

If future implementations allow parallel callback execution, callbacks must be
refactored to heap-allocate returns.

#### Implementation Files

```
include/core/command_router.h    # Pending request API
src/core/command_router.c        # Registry + routing implementation
```

**Modified Files:**
```
src/mosquitto_comms.c            # on_message routing logic
src/core/worker_pool.c           # Command processing with request/response
```

#### 7. Integration with Existing Code

**Modified `on_message()` Flow:**

```c
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
   json_object *parsed = json_tokener_parse(msg->payload);

   // Check for request_id (worker request)
   json_object *request_id_obj;
   const char *request_id = NULL;
   if (json_object_object_get_ex(parsed, "request_id", &request_id_obj)) {
      request_id = json_object_get_string(request_id_obj);
   }

   if (request_id) {
      // WORKER PATH: Execute callback, route result to worker
      execute_command_for_worker(parsed, request_id);
   } else {
      // LOCAL PATH: Existing flow (executes callback + calls LLM)
      parseJsonCommandandExecute(msg->payload);
   }

   json_object_put(parsed);
}

static void execute_command_for_worker(json_object *cmd, const char *request_id) {
   // Extract device/action/value from JSON
   json_object *device_obj, *action_obj, *value_obj;
   json_object_object_get_ex(cmd, "device", &device_obj);
   json_object_object_get_ex(cmd, "action", &action_obj);
   json_object_object_get_ex(cmd, "value", &value_obj);

   const char *device = json_object_get_string(device_obj);
   const char *action = json_object_get_string(action_obj);
   const char *value = value_obj ? json_object_get_string(value_obj) : NULL;

   // Find and execute callback
   char *result = NULL;
   int should_respond = 0;
   for (int i = 0; i < MAX_DEVICE_TYPES; i++) {
      if (strcmp(device, deviceTypeStrings[i]) == 0) {
         result = deviceCallbackArray[i].callback(action, (char *)value, &should_respond);
         break;
      }
   }

   // Route result to waiting worker
   if (result && should_respond) {
      command_router_deliver(request_id, result);
      // Note: result is static buffer, strdup'd in deliver()
   } else {
      // Deliver empty result (command executed but no data returned)
      command_router_deliver(request_id, "");
   }
}
```

#### 8. Error Handling

**Registry Full:**
```c
pending_request_t *req = command_router_register(ctx->worker_id);
if (!req) {
   // Registry full - skip command execution, log warning
   remove_command_tag(tts_text, cmd_start, cmd_end);
   LOG_WARNING("Worker %d: Command registry full, skipping command", ctx->worker_id);
   continue;  // Process next command tag
}
```

**Worker Disconnect During Wait:**
Add cleanup in `worker_pool_shutdown()`:
```c
// Cancel all pending requests for shutting-down workers
command_router_cancel_all_for_worker(worker_id);
```

**Dynamic Buffer for Substitution:**
```c
// Use dynamic buffer to handle variable-length results
char *tts_text = strdup(llm_response);
size_t tts_capacity = strlen(llm_response) + 1;

// In substitution loop:
size_t result_len = strlen(result);
size_t tag_len = (cmd_end + strlen("</command>")) - cmd_start;
size_t needed = strlen(tts_text) - tag_len + result_len + 1;

if (needed > tts_capacity) {
   tts_capacity = needed + 256;  // Extra headroom
   tts_text = realloc(tts_text, tts_capacity);
}
```

#### Testing

```bash
# Unit test: command router
./tests/test_command_router

# Integration: worker gets time result
# 1. Start dawn
# 2. Send DAP audio: "What time is it?"
# 3. Verify client receives TTS with actual time (not command JSON)

# External service test:
# 1. Start mock search service
# 2. Send DAP audio: "Search for Python tutorials"
# 3. Verify search results appear in TTS response

# Edge case tests:
# - Registry full behavior
# - Timeout handling (slow/offline external service)
# - Worker disconnect during wait
# - Multiple concurrent workers requesting same device
```

---

### Phase 4: Robustness + Metrics ✅ COMPLETE

**Goal**: Production-ready error handling.

**Status**: All items implemented and verified.

1. ✅ **Per-session LLM cancellation**: `session->disconnected` flag checked at 5+ points in pipeline
2. ✅ **LLM timeout (30s)**: CURL progress callback + configurable `llm_timeout_ms` + low-speed fallback
3. ✅ **Client disconnect detection**: Sets disconnected flag, triggers cleanup
4. ✅ **Worker crash recovery**: `worker_cleanup_handler()` + error TTS fallbacks at each pipeline step
5. ✅ **Metrics system**: Comprehensive `dawn_metrics_t` with 40+ recording functions
6. ✅ **MQTT callbacks**: Heap-allocated returns (documented contract in mosquitto_comms.c)
7. ✅ **TTS PCM interface**: `text_to_speech_to_pcm()` implemented
   ```c
   // include/tts/text_to_speech.h
   int text_to_speech_to_pcm(const char *text,
                             int16_t **pcm_data_out,
                             size_t *pcm_samples_out,
                             uint32_t *sample_rate_out);
   ```
   - Uses `piper::textToAudio()` directly for raw PCM (no WAV header overhead)
   - `text_to_speech_to_wav()` refactored to wrap `text_to_speech_to_pcm()`
   - Thread-safe via existing `tts_mutex`

**Test**: Disconnect mid-processing doesn't leak resources, LLM call aborts cleanly

### Phase 5: DAP2 Protocol Support

**Goal**: Support DAP 2.0 satellites with tiered capabilities (aligns with DAP2_DESIGN.md Phase 1-5).

1. **Implement DAP2 protocol library** (from `common/protocol/dap2_common.c`):
   - Packet encode/decode (14-byte header per DAP2_DESIGN.md Section 5.1)
   - CRC-16 checksum validation
   - **Header file** `include/protocol/dap2.h`:
   ```c
   // DAP2 magic bytes (first 2 bytes of every packet)
   #define DAP2_MAGIC_0 0xDA
   #define DAP2_MAGIC_1 0x02

   // Message types (per DAP2_DESIGN.md Section 5.2)
   #define DAP2_MSG_REGISTER       0x01  // Satellite → Daemon: identity + capabilities
   #define DAP2_MSG_REGISTER_ACK   0x02  // Daemon → Satellite: registration confirmed
   #define DAP2_MSG_QUERY          0x10  // Tier 1: text query
   #define DAP2_MSG_QUERY_AUDIO    0x11  // Tier 2: ADPCM audio query
   #define DAP2_MSG_RESPONSE_STREAM 0x21 // Tier 1: streamed text chunk
   #define DAP2_MSG_RESPONSE_AUDIO  0x22 // Tier 2: ADPCM audio response chunk
   #define DAP2_MSG_RESPONSE_END   0x23  // Both: end of response stream
   #define DAP2_MSG_HEARTBEAT      0x30  // Keep-alive
   #define DAP2_MSG_NACK           0xFF  // Error response

   // Flags (bit field in header)
   #define DAP2_FLAG_STREAMING     0x01  // More chunks follow
   #define DAP2_FLAG_FINAL         0x02  // Last chunk in stream

   // Error codes (payload of NACK message)
   #define DAP2_ERR_INVALID_MSG_TYPE   0x01
   #define DAP2_ERR_INVALID_TIER       0x02
   #define DAP2_ERR_CHECKSUM_FAILED    0x03
   #define DAP2_ERR_SESSION_LIMIT      0x04
   #define DAP2_ERR_INTERNAL           0xFF

   // --- Network I/O Functions ---

   // Receive text query from Tier 1 satellite
   // Returns: Allocated string (caller frees), or NULL on error
   char *dap2_recv_query_text(int fd);

   // Receive audio query from Tier 2 satellite
   // Returns: 0 on success, -1 on error
   int dap2_recv_query_audio(int fd, uint8_t **adpcm_out, size_t *size_out);

   // Send text response (single packet or chunked if large)
   int dap2_send_response_text(int fd, const char *text);

   // Send audio response (auto-chunks if needed)
   int dap2_send_response_audio(int fd, const uint8_t *adpcm, size_t size);

   // Generic packet send
   int dap2_send_packet(int fd, uint8_t msg_type, const void *payload, size_t len);

   // Send NACK with error code
   int dap2_send_nack(int fd, uint8_t error_code);
   ```

2. **Implement DAP2 server** in accept thread:
   - Detect DAP2 magic bytes (0xDA 0x02) vs legacy DAP1
   - Parse REGISTER message → extract tier, identity, capabilities
   - Create/restore session by UUID (reconnection support)
   - Route to worker with session context

3. **Implement tier-based worker branching** (see "DAP2 Tier-Based Pipeline Branching" above):
   - Tier 1: TEXT → LLM → TEXT (RESPONSE_STREAM for streaming)
   - Tier 2: ADPCM → ASR → LLM → TTS → ADPCM

4. **ADPCM codec** for Tier 2 audio:
   - IMA ADPCM encode/decode (4:1 compression)
   - 16-bit PCM @ 16kHz → 4-bit ADPCM @ 32kbps
   - **Stateful codec**: IMA ADPCM maintains predictor state between blocks for quality
   - API specification:
   ```c
   // include/protocol/adpcm.h

   /**
    * ADPCM codec with per-worker state context.
    *
    * IMA ADPCM is STATEFUL - it maintains a predictor value across blocks.
    * Each worker needs its own state to avoid cross-talk and ensure quality.
    *
    * Thread safety:
    * - adpcm_state_t is NOT thread-safe
    * - Each worker creates/owns its own state (no sharing)
    * - Different workers can encode/decode concurrently with their own state
    */

   typedef struct adpcm_state adpcm_state_t;

   // Create ADPCM state (one per worker, created at worker init)
   adpcm_state_t *adpcm_state_init(void);
   void adpcm_state_cleanup(adpcm_state_t *state);

   // Reset state for new audio stream (call between utterances)
   void adpcm_state_reset(adpcm_state_t *state);

   // Encode PCM samples to ADPCM (uses and updates predictor state)
   // Returns: SUCCESS (0) or FAILURE (1)
   // Ownership: Caller must free *adpcm_out
   int adpcm_encode(adpcm_state_t *state, const int16_t *pcm, size_t pcm_samples,
                    uint8_t **adpcm_out, size_t *adpcm_size_out);

   // Decode ADPCM to PCM samples (uses and updates predictor state)
   // Returns: SUCCESS (0) or FAILURE (1)
   // Ownership: Caller must free *pcm_out
   int adpcm_decode(adpcm_state_t *state, const uint8_t *adpcm, size_t adpcm_size,
                    int16_t **pcm_out, size_t *pcm_samples_out);
   ```
   - Worker integration: State created in `worker_pool_init()`, reset between requests

5. **TTS PCM output** for Tier 2:
   - Add `text_to_speech_to_pcm()` to TTS interface (avoids WAV header overhead)
   ```c
   // include/tts/text_to_speech.h (addition)
   int text_to_speech_to_pcm(const char *text, int16_t **pcm_out, size_t *samples_out);
   ```

6. **Response streaming** for lower perceived latency:
   - Tier 1: Send full text via `dap2_send_response_text()`, satellite splits locally
   - Tier 2: Stream RESPONSE_AUDIO chunks for progressive playback
   - Both: RESPONSE_END (0x23) signals completion
   - Satellite-side TTS can begin immediately on first chunk

**Files**:
```
include/protocol/dap2.h           # Protocol constants, message types
src/protocol/dap2_codec.c         # Encode/decode packets
src/protocol/adpcm.c              # IMA ADPCM codec
src/network/dap2_handler.c        # DAP2-specific accept/dispatch logic
```

**Test**:
- Tier 1 satellite (Python mock) sends text, receives streamed text
- Tier 2 satellite (mock) sends ADPCM, receives ADPCM response
- Reconnection preserves conversation history by UUID

**Cross-reference**: See `docs/DAP2_DESIGN.md` for full protocol specification.

---

### Phase 6: WebSocket Support (for WebUI)

**Goal**: Real-time web interface.

1. Add WebSocket protocol handling (libwebsockets or similar)
2. Create WebSocket session type
3. Workers handle WebSocket clients same as DAP
4. Real-time state broadcasting to WebUI

**Test**: WebUI can connect and interact with DAWN

---

## File Structure

```
include/
├── core/
│   ├── session_manager.h    # Session lifecycle and conversation history
│   └── worker_pool.h        # Worker threads with full pipelines
├── network/
│   ├── accept_thread.h      # TCP accept and client dispatch
│   ├── dap2_handler.h       # DAP2 protocol handling (Phase 5)
│   └── websocket_handler.h  # WebSocket protocol (Phase 6)
└── protocol/
    ├── dap2.h               # DAP2 constants, message types, structures
    └── adpcm.h              # IMA ADPCM codec interface

src/
├── core/
│   ├── session_manager.c
│   └── worker_pool.c
├── network/
│   ├── accept_thread.c
│   ├── dap2_handler.c       # DAP2 accept, register, dispatch
│   └── websocket_handler.c
└── protocol/
    ├── dap2_codec.c         # Packet encode/decode, CRC-16
    └── adpcm.c              # IMA ADPCM encode/decode

# Modified existing files:
include/state_machine.h      # Remove DAWN_STATE_NETWORK_PROCESSING
```

**Note**: Simpler than queued design - no request_queue or request_processor needed.

**DAP2 Protocol Files** (see `docs/DAP2_DESIGN.md` Section 6 for shared code architecture):
- Protocol codec goes in `src/protocol/` (daemon-specific initially)
- Common code can be factored to `common/protocol/` when implementing Tier 1 satellites

---

## Testing Strategy

### Unit Tests

```bash
# Build test binaries
cd tests
make test_session_manager test_worker_pool test_vosk_concurrent

# Run with local LLM (no API costs)
export OPENAI_API_BASE=http://localhost:11434/v1  # Ollama
export OPENAI_MODEL=llama3.2

./test_session_manager
./test_worker_pool
./test_asr_concurrent  # Critical: verify multi-context works
```

**Test Cases**:
- Session manager: create, get, destroy, expiry, reference counting
- Worker pool: start, shutdown, client assignment, full pipeline execution
- ASR concurrent: Multiple ASR contexts (Vosk or Whisper) work in parallel
- TTS generate: WAV output is valid and playable

### Integration Tests

```bash
# Mock DAP client for automated testing
./tests/mock_dap_client --host localhost --port 5000 --audio test.wav --expect-response

# Concurrent client test (the key test!)
./tests/multi_client_test --clients 4 --duration 60

# DAP2 protocol tests
./tests/mock_dap2_tier1 --host localhost --port 5000 --text "turn on the lights"
./tests/mock_dap2_tier2 --host localhost --port 5000 --audio test.wav --expect-adpcm
```

**Test Cases**:
- Local voice: Full flow still works (unchanged)
- Network client: DAP client connects, sends audio, receives response
- Concurrent: 4 clients conversing simultaneously, independent histories
- Parallel LLM: Multiple LLM calls in flight, all complete successfully

### DAP2 Protocol Tests

```bash
# Python test client for DAP2 (from DAP2_DESIGN.md Phase 1)
python3 tests/dap2_test_client.py --tier 1 --text "what time is it"
python3 tests/dap2_test_client.py --tier 2 --audio test_adpcm.bin
```

**Test Cases**:
- REGISTER: Tier 1 and Tier 2 registration, capability negotiation
- QUERY (Tier 1): Text query → streamed text response
- QUERY_AUDIO (Tier 2): ADPCM upload → ADPCM download
- Reconnection: Same UUID reconnects, gets existing session with history
- Streaming: RESPONSE_STREAM chunks arrive in correct order
- ADPCM quality: Round-trip audio intelligible (PESQ >3.0 if measured)

### Stress Tests

- Max sessions reached (should reject cleanly)
- All workers busy (new clients wait or get rejected)
- Client disconnect mid-processing (cleanup resources, worker continues)
- Concurrent LLM calls (verify LLM server handles load)

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Deadlocks | Low | High | Simple lock hierarchy, workers mostly independent |
| Resource leaks | Medium | Medium | Session expiry, cleanup on disconnect |
| Race conditions | Low | High | Per-client pipelines minimize shared state |
| Performance regression | Low | Medium | Benchmark before/after |
| ASR multi-context issues | Low | High | Verify in Phase 1 before Phase 2 (per `asr_interface.h` docs) |
| LLM server overload | Medium | Medium | Worker pool size limits concurrent calls |

**Note**: Per-client pipelines significantly reduce threading risks compared to queued design.

---

## Resolved Questions

| Question | Resolution |
|----------|------------|
| Parallel LLM processing? | Yes - each worker makes independent LLM calls |
| Session persistence? | No - sessions are transient, restart clears all |
| Priority handling? | Implicit - local uses existing fast path, workers independent |
| Graceful degradation? | Reject new clients when all workers busy |

---

## Design Evolution Notes

### Original Design (Queued)
- Central request queue bottleneck
- Single processor thread for all LLM calls
- Complex priority queue management

### Final Design (Per-Client Pipelines)
- No central queue - workers handle full pipeline
- Parallel LLM calls (limited by worker pool size)
- Simpler ownership - each worker owns its data flow
- Lower lock contention - workers rarely synchronize

**Key insight from user**: "No reason to have a bottleneck anywhere in our processing, so we can scale with hardware."

---

## Document History

- 2025-12-05: Initial design draft
- 2025-12-05: Revised after architecture review - addressed critical issues
- 2025-12-05: Redesigned to per-client pipelines - eliminated central queue bottleneck
- 2025-12-05: Generalized ASR references to use `asr_interface.h` abstraction (supports Vosk or Whisper)
- 2025-12-05: Fixed session ref counting (two-phase destruction), lock ordering, shutdown timeout, session cleanup scheduling. Corrected TTS function to existing `text_to_speech_to_wav()`
- 2025-12-05: Added DAP2 protocol extensions:
  - Session structure: tier, UUID, identity, capabilities fields
  - Tier-based pipeline branching (Tier 1: text, Tier 2: ADPCM audio)
  - Phase 5: DAP2 protocol support (replaces previous Phase 5)
  - Phase 6: WebSocket support (renumbered)
  - Response streaming for lower latency (RESPONSE_STREAM)
  - ADPCM codec for Tier 2 audio compression
  - Cross-references to DAP2_DESIGN.md
- 2025-12-05: Architecture review fixes:
  - Added fd_mutex to session for reconnection safety (#1)
  - Added ADPCM codec API spec (#2)
  - Changed Tier 2 to use text_to_speech_to_pcm() (#3)
  - Added tier validation in worker dispatch (#4)
  - Added streaming protocol details (sentence_split, RESPONSE_END) (#5)
  - Removed session_find_by_uuid(), made session_create_dap2() atomic (#6)
  - Updated lock ordering (6 levels now)
- 2025-12-05: Second architecture review fixes:
  - Simplified lock analysis: explained exclusive ownership makes per-session locks largely unnecessary
  - Added ADPCM state context (stateful codec with per-worker state)
  - Moved text_to_speech_to_pcm() to Phase 4 deliverables (dependency for Phase 5)
  - Added dap2.h header spec with message type constants and error codes
- 2025-12-05: Third architecture review fixes (final):
  - C1: Simplified Tier 1 handler - send full text, satellite splits sentences locally
  - C2: Clarified TTS dependency - text_to_speech_to_pcm() noted as Phase 4 implementation
  - C3: Added DAP2 network I/O function specs to dap2.h header
  - C4: Added adpcm_state to worker_context_t for per-worker codec state
  - I3: Added session_llm_call() helper for LLM calls with session context
  - I5: Accept thread uses select() with 60s timeout for periodic cleanup
  - I8: Added detailed state machine removal checklist in Phase 2
- 2025-12-05: Final review fix:
  - I1: Clarified worker_thread() is pseudocode, worker_thread_dap2() is actual implementation
- 2025-12-08: Phase 3 implementation:
  - Per-session conversation history migrated from global
  - Added `session_init_system_prompt()` for session initialization
  - Fixed shutdown bug: self-pipe pattern for reliable accept thread wakeup
  - Fixed segfault: proper session_manager_cleanup ownership (dawn.c owns)
  - Added DAP1 IP-based session persistence (`session_get_or_create_dap()`)
  - Added remote vs local system prompts: `get_remote_command_prompt()` excludes
    HUD/helmet topics for network satellites, keeping local-only commands secure
- 2025-12-08: Added Phase 3.5 - MQTT Request/Response for Command Results:
  - Enables worker threads to receive command callback results via MQTT
  - Pending request registry with condition variable synchronization
  - Request/response pattern preserves existing MQTT architecture
  - Optional multi-turn LLM integration for complex results (search, weather)
  - Future extension path for external services (Python search service via MQTT)
  - Solves "remote client hears command JSON instead of actual time" issue
- 2025-12-08: Phase 3.5 architecture review fixes:
  - C1: Fixed race condition - `registry_mutex` held throughout delivery operation
  - C2: Protected registry lookup - search happens while holding `registry_mutex`
  - C3: Documented explicit lock ordering rules with rationale
  - I1: Added worker disconnect cleanup via `command_router_cancel_all_for_worker()`
  - I2: Documented static callback buffer safety (serialized by MQTT thread)
  - I3: Added `execute_command_for_worker()` integration with existing code
  - I4: Added registry full error handling (skip command, log warning)
  - I5: Added dynamic buffer with realloc for variable-length substitutions
- 2025-12-15: Updated document status - Phases 1-3.5 implemented, Phase 4 next
  - Added cross-reference to WEBUI_DESIGN.md (Phase 6 prerequisite)
- 2025-12-15: Phase 4 marked complete
  - All robustness features were already implemented (LLM cancellation, timeout, metrics, etc.)
  - Added `text_to_speech_to_pcm()` using `piper::textToAudio()` for raw PCM output
  - Refactored `text_to_speech_to_wav()` to wrap `text_to_speech_to_pcm()` (no code duplication)
  - Ready for Phase 5 (DAP2) or Phase 6 (WebUI)

