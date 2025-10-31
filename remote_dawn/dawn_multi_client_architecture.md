# DAWN Multi-Client Architecture Design

## Executive Summary

Recommended architecture for handling concurrent local and remote audio processing in DAWN using a hybrid threading model. This preserves existing local audio functionality while enabling true concurrency for multiple ESP32 network clients.

## Current Limitations

- `NETWORK_PROCESSING` state blocks entire main loop during processing
- Only one network client can be processed at a time
- Local microphone input ignored during network processing (10-15 seconds during LLM)
- Second client must wait or gets echo fallback

## Recommended Solution: Hybrid Threading Model

### Main Thread
- Keeps existing state machine for local microphone processing
- Handles "user sitting at the computer" mode
- No changes to SILENCE → WAKEWORD_LISTEN → COMMAND_RECORDING flow

### Worker Thread Pool
- Each ESP32 connection spawns or uses a worker thread
- True concurrency for remote clients
- Independent conversation contexts per client
- No blocking of local audio processing

## Implementation Details

### 1. Client Session Management

```c
typedef struct {
   char client_id[64];              // IP address or UUID
   struct json_object *conversation_history;
   pthread_mutex_t conversation_mutex;
   time_t last_activity;
   int is_active;
} ClientSession;

// Global client registry
typedef struct {
   ClientSession **sessions;
   int max_sessions;
   int active_count;
   pthread_mutex_t registry_mutex;
} ClientRegistry;
```

### 2. Worker Thread Architecture

Modify `dawn_handle_client_connection()` to spawn worker threads:

```c
// In dawn_server.c
static void *client_worker_thread(void *arg) {
   ClientContext *ctx = (ClientContext*)arg;
   
   // Get or create client session
   ClientSession *session = get_or_create_client_session(ctx->client_ip);
   
   // Process audio with client-specific context
   process_client_audio(ctx, session);
   
   // Update last activity
   session->last_activity = time(NULL);
   
   free(ctx);
   return NULL;
}

// In accept loop
ClientContext *ctx = malloc(sizeof(ClientContext));
// ... populate ctx ...

pthread_t worker;
pthread_create(&worker, NULL, client_worker_thread, ctx);
pthread_detach(worker);  // Fire and forget
```

### 3. Shared Resources (Already Thread-Safe)

✓ **Vosk Model** - Read-only, can be shared. Create separate recognizers per thread:
```c
VoskRecognizer *recognizer = vosk_recognizer_new(shared_model, 16000);
// ... use it ...
vosk_recognizer_free(recognizer);
```

✓ **LLM (LocalAI/llama.cpp)** - Already handles concurrent HTTP requests

✓ **TTS (`text_to_speech_to_wav`)** - Already locks `tts_mutex`, safe for concurrent calls

### 4. Simplified Callback Processing

Remove `NETWORK_PROCESSING` state entirely. Process directly in worker thread:

```c
// NEW: Process directly in worker thread
uint8_t* dawn_process_network_audio(const uint8_t *audio_data, 
                                    size_t audio_size,
                                    const char *client_info,
                                    size_t *response_size) {
   ClientSession *session = get_client_session(client_info);
   
   // Extract PCM
   NetworkPCMData *pcm = extract_pcm_from_network_wav(audio_data, audio_size);
   if (!pcm || !pcm->is_valid) {
      return error_to_wav(ERROR_MSG_WAV_INVALID, response_size);
   }
   
   // Speech recognition
   VoskRecognizer *recognizer = vosk_recognizer_new(shared_vosk_model, 16000);
   vosk_recognizer_accept_waveform(recognizer, (const char*)pcm->pcm_data, pcm->pcm_size);
   const char *vosk_result = vosk_recognizer_final_result(recognizer);
   char *transcription = getTextResponse(vosk_result);
   vosk_recognizer_free(recognizer);
   
   if (!transcription || strlen(transcription) == 0) {
      free_network_pcm_data(pcm);
      return error_to_wav(ERROR_MSG_SPEECH_FAILED, response_size);
   }
   
   // LLM processing
   char *response = getGptResponse(session->conversation_history, transcription, NULL, 0);
   free(transcription);
   free_network_pcm_data(pcm);
   
   if (!response) {
      return error_to_wav("I'm sorry but I'm currently unavailable.", response_size);
   }
   
   // Clean response
   remove_chars(response, "*");
   remove_emojis(response);
   
   // Generate TTS
   uint8_t *wav = NULL;
   if (text_to_speech_to_wav(response, &wav, response_size) != 0) {
      free(response);
      return error_to_wav("Failed to generate response.", response_size);
   }
   
   // Check size limits and truncate if needed
   if (!check_response_size_limit(*response_size)) {
      uint8_t *truncated = NULL;
      size_t truncated_size = 0;
      if (truncate_wav_response(wav, *response_size, &truncated, &truncated_size) == 0) {
         free(wav);
         wav = truncated;
         *response_size = truncated_size;
      }
   }
   
   free(response);
   return wav;
}
```

## Client ID Strategy

**Primary:** Use IP address as client ID
```c
char client_id[64];
snprintf(client_id, sizeof(client_id), "%s", session.client_ip);
```

**Special ID for local audio:** `"local"` or `"127.0.0.1:local"`

**Alternative:** Generate UUIDs if same client might reconnect with different IPs

## Session Management

### Session Lifecycle

1. **Create** - First connection from client creates new session
2. **Reuse** - Subsequent connections from same IP reuse session (maintains conversation)
3. **Timeout** - Inactive sessions cleaned up after 10 minutes
4. **Explicit Close** - Optional: Add "goodbye" command to explicitly end session

### Cleanup Thread

```c
void cleanup_inactive_sessions(ClientRegistry *registry) {
   time_t now = time(NULL);
   pthread_mutex_lock(&registry->registry_mutex);
   
   for (int i = 0; i < registry->max_sessions; i++) {
      ClientSession *s = registry->sessions[i];
      if (s && s->is_active && (now - s->last_activity) > 600) {  // 10 min
         LOG_INFO("Cleaning up inactive session: %s", s->client_id);
         json_object_put(s->conversation_history);
         pthread_mutex_destroy(&s->conversation_mutex);
         free(s);
         registry->sessions[i] = NULL;
         registry->active_count--;
      }
   }
   
   pthread_mutex_unlock(&registry->registry_mutex);
}

// Spawn cleanup thread at startup
pthread_t cleanup_thread;
pthread_create(&cleanup_thread, NULL, session_cleanup_thread, &client_registry);
pthread_detach(cleanup_thread);
```

## Migration Path

### Phase 1: Add Session Management
- Create `ClientSession` and `ClientRegistry` structures
- Implement `get_or_create_client_session()`
- Add session registry initialization in `dawn_network_audio_init()`

### Phase 2: Worker Thread Implementation
- Modify `dawn_handle_client_connection()` to spawn workers
- Move audio processing logic into worker function
- Test with single client first

### Phase 3: Remove State Machine Dependency
- Remove `NETWORK_PROCESSING` case from main state machine
- Remove IPC synchronization (`processing_mutex`, `processing_done`, etc.)
- Simplify `dawn_process_network_audio()` callback

### Phase 4: Session Cleanup
- Add session timeout cleanup thread
- Implement session management commands (optional)
- Add monitoring/logging for active sessions

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────┐
│                     DAWN SERVER                         │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Main Thread                 Worker Thread Pool         │
│  ┌──────────────┐           ┌──────────────────┐      │
│  │ Local Audio  │           │  ESP32 Client 1  │      │
│  │ State Machine│           │  (192.168.1.50)  │      │
│  │              │           │  conversation: 5 │      │
│  │ SILENCE      │           └──────────────────┘      │
│  │ WAKEWORD     │           ┌──────────────────┐      │
│  │ RECORDING    │           │  ESP32 Client 2  │      │
│  │ VISION_AI    │           │  (192.168.1.51)  │      │
│  └──────────────┘           │  conversation: 3 │      │
│        │                    └──────────────────┘      │
│        │                    ┌──────────────────┐      │
│        │                    │  ESP32 Client 3  │      │
│        │                    │  (192.168.1.52)  │      │
│        │                    │  conversation: 1 │      │
│        │                    └──────────────────┘      │
│        └────────────┬──────────────┬──────────┘       │
│                     ↓              ↓                   │
│            ┌─────────────────────────────┐            │
│            │    Shared Resources         │            │
│            │  - Vosk Model (shared)      │            │
│            │  - LLM Endpoint (HTTP)      │            │
│            │  - TTS Engine (mutex-safe)  │            │
│            │  - Client Registry          │            │
│            └─────────────────────────────┘            │
└─────────────────────────────────────────────────────────┘
```

## Benefits

1. **Non-blocking Local Audio** - Local microphone always responsive
2. **True Concurrency** - Multiple ESP32 clients processed simultaneously
3. **Resource Isolation** - Each client maintains independent conversation
4. **Scalability** - Easy to add connection pooling, rate limiting
5. **Code Simplification** - Removes complex IPC synchronization
6. **Maintainability** - Clear separation of concerns

## Potential Issues & Solutions

### Issue: Thread Pool Exhaustion
**Solution:** Implement max connections limit + connection queue

### Issue: Memory Growth
**Solution:** Session timeout + periodic cleanup thread

### Issue: Vosk Model Memory
**Solution:** Model is read-only, safely shared. Create recognizers per-thread.

### Issue: Race Conditions in Session Access
**Solution:** Per-session mutex protects conversation_history

### Issue: Client Reconnection
**Solution:** Session timeout handles this - new connection gets new context after timeout

## Testing Strategy

1. **Single client** - Verify worker thread spawns correctly
2. **Multiple clients** - Test concurrent processing (3-5 clients)
3. **Local + network** - Verify main thread isn't blocked
4. **Session persistence** - Test conversation continuity across reconnects
5. **Session cleanup** - Verify inactive sessions are freed
6. **Load testing** - Stress test with 10+ concurrent clients

## Configuration Recommendations

```c
#define MAX_CLIENT_SESSIONS 32
#define SESSION_TIMEOUT_SEC 600  // 10 minutes
#define CLEANUP_INTERVAL_SEC 300  // 5 minutes
```

## Future Enhancements

- Connection pooling for worker threads
- Client authentication/authorization
- Per-client rate limiting
- Session persistence to disk
- Conversation export/import
- Multi-server architecture with shared session store

---

**Document Version:** 1.0  
**Date:** 2025-10-30  
**Author:** Code Review Discussion
