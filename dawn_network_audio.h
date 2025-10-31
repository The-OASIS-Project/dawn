/*
 * DAWN Network Audio - IPC Bridge for Network Client Processing
 *
 * PURPOSE:
 *   Provides Inter-Process Communication (IPC) between the network server
 *   thread and the main application's state machine thread for processing
 *   audio from remote ESP32 clients.
 *
 * ARCHITECTURE:
 *   - Server thread receives audio from ESP32 client
 *   - Callback (dawn_process_network_audio) stores audio in global buffer
 *   - Main thread polls for network_audio_ready flag
 *   - Main thread processes audio in NETWORK_PROCESSING state
 *   - Main thread stores result in processing_result_data global
 *   - Callback thread wakes up and returns result to server
 *   - Server thread sends result to ESP32 and frees memory
 *
 * THREADING MODEL:
 *   This is a SINGLE-CLIENT design. Only one network client can be
 *   processed at a time. The callback blocks the server thread waiting
 *   for the main thread to complete processing.
 *
 * LIMITATIONS:
 *   - Blocks main state machine during network processing
 *   - Only one client can be serviced at a time
 *   - Second client gets echo fallback if first is still processing
 *   - Local microphone input ignored during network processing
 *
 * FUTURE WORK:
 *   See dawn_multi_client_architecture.md for the worker thread design
 *   that will replace this IPC mechanism.
 *
 * MEMORY OWNERSHIP:
 *   - network_audio_buffer: Owned by this module, freed on cleanup
 *   - processing_result_data: Allocated in dawn.c, freed in dawn_server.c
 *                             (passed through this module as pointer)
 *
 * SYNCHRONIZATION:
 *   - network_audio_mutex: Protects network_audio_buffer and related state
 *   - processing_mutex: Protects processing_result_data and completion flag
 *   - processing_done: Condition variable signaled by main thread
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 */

#ifndef DAWN_NETWORK_AUDIO_H
#define DAWN_NETWORK_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <signal.h>

// Include the server header for callback types
#include "dawn_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// === Processing Synchronization ===

/**
 * Mutex for protecting processing result variables
 * SCOPE: Protects processing_result_data, processing_result_size, 
 *        processing_complete
 * DEFINED IN: dawn.c (main application)
 */
extern pthread_mutex_t processing_mutex;

/**
 * Condition variable signaled when main thread completes processing
 * SIGNAL: Main thread signals after storing result in processing_result_data
 * WAIT:   Server thread waits in dawn_process_network_audio()
 * DEFINED IN: dawn.c (main application)
 */
extern pthread_cond_t processing_done;

/**
 * Pointer to processed audio result (TTS WAV data)
 *
 * LIFECYCLE:
 *   1. Main thread allocates via text_to_speech_to_wav()
 *   2. Main thread stores pointer here
 *   3. Server thread retrieves pointer
 *   4. Server thread frees after sending to client
 *
 * PROTECTION: Access must be protected by processing_mutex
 * DEFINED IN: dawn.c (main application)
 */
extern uint8_t *processing_result_data;

/**
 * Size of data pointed to by processing_result_data (bytes)
 * PROTECTION: Access must be protected by processing_mutex
 * DEFINED IN: dawn.c (main application)
 */
extern size_t processing_result_size;

/**
 * Flag indicating processing is complete
 * 0 = Processing in progress or not started
 * 1 = Processing complete, result available
 * PROTECTION: Access must be protected by processing_mutex
 * DEFINED IN: dawn.c (main application)
 */
extern int processing_complete;

// === Configuration ===

/**
 * Timeout for network audio processing (seconds)
 * If main thread doesn't complete processing within this time,
 * the callback returns an echo fallback to the client
 */
#define NETWORK_PROCESSING_TIMEOUT_SEC 30

// === Public API ===

/**
 * Initialize the network audio system
 * This must be called before starting the server
 * 
 * @return 0 on success, -1 on error
 */
int dawn_network_audio_init(void);

/**
 * Cleanup the network audio system
 * This should be called when shutting down
 */
void dawn_network_audio_cleanup(void);

/**
 * Audio processor callback - IPC bridge between server and main thread
 *
 * EXECUTION MODEL:
 *   1. Server thread receives audio from ESP32 client
 *   2. Server thread calls this function (entry point)
 *   3. Audio is copied to global buffer (network_audio_buffer)
 *   4. Flag is set to notify main thread (network_audio_ready = 1)
 *   5. **THIS FUNCTION BLOCKS** waiting for main thread
 *   6. Main thread detects flag in state machine loop
 *   7. Main thread transitions to NETWORK_PROCESSING state
 *   8. Main thread processes: Vosk → LLM → TTS
 *   9. Main thread stores result in processing_result_data
 *  10. Main thread signals condition variable (processing_done)
 *  11. This function wakes up and returns result pointer
 *  12. Server thread sends result to ESP32 and frees pointer
 *
 * BLOCKING BEHAVIOR:
 *   This function BLOCKS the server thread for up to
 *   NETWORK_PROCESSING_TIMEOUT_SEC seconds waiting for the main
 *   thread to complete processing. During this time, no new
 *   client connections can be accepted.
 *
 * SINGLE CLIENT LIMITATION:
 *   Only one client can be processed at a time due to global
 *   buffers. If called while another client is processing, the
 *   previous client's data will be overwritten (race condition).
 *
 * MEMORY OWNERSHIP:
 *   INPUT:  audio_data is owned by caller, not modified or freed
 *   OUTPUT: Return value is allocated in dawn.c by main thread
 *           CALLER (dawn_server.c) MUST free using free()
 *
 *   Memory flow: dawn.c (allocate) → this function (pass) →
 *                dawn_server.c (free)
 *
 * ERROR HANDLING:
 *   - Returns echo of input on timeout (malloc'd copy)
 *   - Returns NULL on allocation failure
 *   - Always logs reason for fallback/failure
 *
 * @param audio_data    [IN]  Received WAV audio (read-only)
 * @param audio_size    [IN]  Size of audio_data in bytes
 * @param client_info   [IN]  Client identifier string (typically IP)
 * @param response_size [OUT] Size of returned audio in bytes
 *
 * @return Pointer to response WAV audio (caller must free)
 *         NULL on allocation failure (response_size set to 0)
 *
 * @warning BLOCKS calling thread for up to 30 seconds
 * @warning Single client only - not thread-safe for concurrent calls
 * @warning Caller MUST free returned pointer with free()
 *
 * @see dawn.c:NETWORK_PROCESSING for main thread processing
 * @see dawn_server.c:dawn_handle_client_connection for caller
 */
uint8_t* dawn_process_network_audio(const uint8_t *audio_data, 
                                   size_t audio_size,
                                   const char *client_info,
                                   size_t *response_size);

/**
 * Check if network audio is available and retrieve it
 *
 * Called by main thread in state machine loop to poll for
 * network audio that needs processing.
 *
 * USAGE PATTERN:
 *   while (!quit) {
 *     if (dawn_get_network_audio(&audio, &size, info)) {
 *       // Process audio
 *       dawn_clear_network_audio();
 *     }
 *   }
 *
 * RETURNED POINTERS:
 *   The returned pointers point to internal buffers.
 *   DO NOT free them! Call dawn_clear_network_audio() instead.
 *
 * @param audio_data_out  [OUT] Pointer to internal buffer (do not free)
 * @param audio_size_out  [OUT] Size of audio data in bytes
 * @param client_info_out [OUT] Buffer for client info (min 64 bytes)
 *                              Optional - can be NULL
 *
 * @return 1 if network audio available (pointers populated)
 *         0 if no network audio ready
 *
 * @warning Returned pointers are internal - do not free
 * @warning Call dawn_clear_network_audio() when done processing
 */
int dawn_get_network_audio(uint8_t **audio_data_out, size_t *audio_size_out, char *client_info_out);

/**
 * Clear the network audio buffer and reset state
 *
 * MUST be called by main thread after processing network audio.
 * Frees the network_audio_buffer and resets the ready flag.
 *
 * THREAD SAFETY:
 *   Thread-safe - uses network_audio_mutex internally
 *
 * WHEN TO CALL:
 *   After completing NETWORK_PROCESSING state in main loop
 */
void dawn_clear_network_audio(void);

#ifdef __cplusplus
}
#endif

#endif // DAWN_NETWORK_AUDIO_H
