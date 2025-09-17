#ifndef DAWN_NETWORK_AUDIO_H
#define DAWN_NETWORK_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <signal.h>

// Include the server header for callback types
#include "dawn_server.h"
#include "dawn_tts_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

// === Network Audio State ===

/**
 * Global flag indicating network audio is ready for processing
 * Use volatile sig_atomic_t for thread-safe access from state machine
 */
extern volatile sig_atomic_t network_audio_ready;

/**
 * Global buffer containing received network audio data
 * Access must be protected by network_audio_mutex
 */
extern uint8_t *network_audio_buffer;

/**
 * Size of data in network_audio_buffer
 * Access must be protected by network_audio_mutex
 */
extern size_t network_audio_size;

/**
 * Mutex for protecting network audio buffer access
 */
extern pthread_mutex_t network_audio_mutex;

/**
 * String identifier for the current client (for logging)
 */
extern char network_client_info[64];

// === Processing Synchronization ===
extern pthread_mutex_t processing_mutex;
extern pthread_cond_t processing_done;
extern uint8_t *processing_result_data;
extern size_t processing_result_size;
extern int processing_complete;

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
 * Audio processor callback function
 * This function is called by the server when audio is received
 * 
 * @param audio_data Pointer to received WAV audio data
 * @param audio_size Size of received audio data in bytes
 * @param client_info String identifying the client (for logging)
 * @param response_size Pointer to store response data size
 * @return Pointer to response audio data (must be freed by caller), or NULL for error
 */
uint8_t* dawn_process_network_audio(const uint8_t *audio_data, 
                                   size_t audio_size,
                                   const char *client_info,
                                   size_t *response_size);

/**
 * Check if network audio is available and get the data
 * This function is called by the main processing loop
 * 
 * @param audio_data_out Pointer to receive the audio data pointer
 * @param audio_size_out Pointer to receive the audio data size
 * @param client_info_out Buffer to receive client info (must be at least 64 bytes)
 * @return 1 if network audio is available, 0 if not
 */
int dawn_get_network_audio(uint8_t **audio_data_out, size_t *audio_size_out, char *client_info_out);

/**
 * Clear the network audio flag and free the buffer
 * This should be called when done processing
 */
void dawn_clear_network_audio(void);

/**
 * Print detailed information about received network audio
 * 
 * @param audio_data Pointer to audio data
 * @param audio_size Size of audio data
 * @param client_info Client information string
 */
void dawn_log_network_audio(const uint8_t *audio_data, size_t audio_size, const char *client_info);

#ifdef __cplusplus
}
#endif

#endif // DAWN_NETWORK_AUDIO_H
