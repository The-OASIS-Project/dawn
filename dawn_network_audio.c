#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "dawn_network_audio.h"
#include "logging.h"
#include "text_to_speech.h"

// Global Variables
volatile sig_atomic_t network_audio_ready = 0;
uint8_t *network_audio_buffer = NULL;
size_t network_audio_size = 0;
pthread_mutex_t network_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
char network_client_info[64] = {0};

// Processing synchronization (external from main application)
extern pthread_mutex_t processing_mutex;
extern pthread_cond_t processing_done;
extern uint8_t *processing_result_data;
extern size_t processing_result_size;
extern int processing_complete;

// Internal state
static int network_audio_initialized = 0;

int dawn_network_audio_init(void) {
    pthread_mutex_lock(&network_audio_mutex);

    if (network_audio_initialized) {
        LOG_INFO("Network audio system already initialized");
        pthread_mutex_unlock(&network_audio_mutex);
        return 0;
    }

    // Initialize global state
    network_audio_ready = 0;
    network_audio_buffer = NULL;
    network_audio_size = 0;
    memset(network_client_info, 0, sizeof(network_client_info));

    network_audio_initialized = 1;

    pthread_mutex_unlock(&network_audio_mutex);

    LOG_INFO("Network audio system initialized successfully");

    // Register our callback with the DAWN server
    dawn_server_set_audio_callback(dawn_process_network_audio);

    return 0;
}

void dawn_network_audio_cleanup(void) {
    pthread_mutex_lock(&network_audio_mutex);

    if (!network_audio_initialized) {
        pthread_mutex_unlock(&network_audio_mutex);
        return;
    }

    // Clear callback
    dawn_server_set_audio_callback(NULL);

    // Free any allocated buffer
    if (network_audio_buffer != NULL) {
        free(network_audio_buffer);
        network_audio_buffer = NULL;
    }

    network_audio_size = 0;
    network_audio_ready = 0;
    network_audio_initialized = 0;

    pthread_mutex_unlock(&network_audio_mutex);

    LOG_INFO("Network audio system cleaned up");
}

uint8_t* dawn_process_network_audio(const uint8_t *audio_data, 
                                          size_t audio_size,
                                          const char *client_info,
                                          size_t *response_size) {
    if (!audio_data || audio_size == 0 || !response_size) {
        LOG_ERROR("Invalid parameters to audio processor");
        return NULL;
    }

    LOG_INFO("Audio processor callback triggered");
    LOG_INFO("Client: %s, received %zu bytes", client_info ? client_info : "unknown", audio_size);

    // Log basic audio information
    dawn_log_network_audio(audio_data, audio_size, client_info);

    pthread_mutex_lock(&network_audio_mutex);

    // Free any existing buffer
    if (network_audio_buffer != NULL) {
        free(network_audio_buffer);
        network_audio_buffer = NULL;
    }

    // Allocate new buffer and copy audio data
    network_audio_buffer = malloc(audio_size);
    if (network_audio_buffer == NULL) {
        LOG_ERROR("Failed to allocate network buffer: %zu bytes", audio_size);
        pthread_mutex_unlock(&network_audio_mutex);
        return NULL;
    }

    memcpy(network_audio_buffer, audio_data, audio_size);
    network_audio_size = audio_size;

    // Store client info for logging
    strncpy(network_client_info, client_info ? client_info : "unknown", sizeof(network_client_info) - 1);
    network_client_info[sizeof(network_client_info) - 1] = '\0';

    LOG_INFO("Network buffer allocated: %zu bytes", audio_size);

    // Set the flag to notify state machine
    network_audio_ready = 1;
    LOG_INFO("Network audio ready for processing");

    pthread_mutex_unlock(&network_audio_mutex);

    // Signal main thread and wait for processing
    pthread_mutex_lock(&network_audio_mutex);
    processing_complete = 0;
    pthread_mutex_unlock(&network_audio_mutex);

    // Wait for main thread to complete processing (with timeout)
    pthread_mutex_lock(&processing_mutex);
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 30; // 30 second timeout

    while (!processing_complete) {
        int result = pthread_cond_timedwait(&processing_done, &processing_mutex, &timeout);
        if (result == ETIMEDOUT) {
            LOG_WARNING("Processing timeout, using echo fallback");
            break;
        }
    }

    uint8_t *result_data = NULL;
    if (processing_complete && processing_result_data && processing_result_size > 0) {
        // Return the processed result (TTS WAV)
        *response_size = processing_result_size;
        result_data = processing_result_data;

        LOG_INFO("Returning processed response: %zu bytes", *response_size);
    } else {
        // Fallback to echo
        result_data = malloc(audio_size);
        if (result_data) {
            memcpy(result_data, audio_data, audio_size);
            *response_size = audio_size;
            LOG_INFO("Returning echo fallback: %zu bytes", *response_size);
        } else {
            LOG_ERROR("Failed to allocate echo fallback buffer");
            *response_size = 0;
        }
    }

    pthread_mutex_unlock(&processing_mutex);
    return result_data;   
}

int dawn_get_network_audio(uint8_t **audio_data_out, size_t *audio_size_out, char *client_info_out) {
    if (!audio_data_out || !audio_size_out) {
        return 0;
    }

    // Check flag first (atomic read)
    if (!network_audio_ready) {
        return 0;
    }

    pthread_mutex_lock(&network_audio_mutex);

    // Double-check flag while holding mutex
    if (!network_audio_ready || network_audio_buffer == NULL) {
        pthread_mutex_unlock(&network_audio_mutex);
        return 0;
    }

    // Return pointers to the data (caller should not free these!)
    *audio_data_out = network_audio_buffer;
    *audio_size_out = network_audio_size;

    if (client_info_out) {
        strncpy(client_info_out, network_client_info, 63);
        client_info_out[63] = '\0';
    }

    LOG_INFO("Retrieved network audio: %zu bytes from %s", 
             network_audio_size, network_client_info);

    pthread_mutex_unlock(&network_audio_mutex);

    return 1;
}

void dawn_clear_network_audio(void) {
    pthread_mutex_lock(&network_audio_mutex);

    if (network_audio_buffer != NULL) {
        free(network_audio_buffer);
        network_audio_buffer = NULL;
    }

    network_audio_size = 0;
    network_audio_ready = 0;
    memset(network_client_info, 0, sizeof(network_client_info));

    LOG_INFO("Network audio cleared");

    pthread_mutex_unlock(&network_audio_mutex);
}

void dawn_log_network_audio(const uint8_t *audio_data, size_t audio_size, const char *client_info) {
    LOG_INFO("Network audio analysis:");
    LOG_INFO("Client: %s, size: %zu bytes", client_info ? client_info : "unknown", audio_size);

    if (audio_data == NULL || audio_size < 44) {
        LOG_WARNING("Audio data too small to be valid WAV (need at least 44 bytes)");
        return;
    }

    // Check if this looks like a WAV file
    if (audio_size >= 12 && 
        audio_data[0] == 'R' && audio_data[1] == 'I' && 
        audio_data[2] == 'F' && audio_data[3] == 'F' &&
        audio_data[8] == 'W' && audio_data[9] == 'A' && 
        audio_data[10] == 'V' && audio_data[11] == 'E') {

        LOG_INFO("WAV format detected");

        if (audio_size >= 44) {
            // Extract basic WAV info (little-endian format)
            uint32_t file_size = *((uint32_t*)(audio_data + 4)) + 8;
            uint16_t audio_format = *((uint16_t*)(audio_data + 20));
            uint16_t num_channels = *((uint16_t*)(audio_data + 22));
            uint32_t sample_rate = *((uint32_t*)(audio_data + 24));
            uint16_t bits_per_sample = *((uint16_t*)(audio_data + 34));

            LOG_INFO("WAV details: %u bytes, %uHz, %u channels, %u-bit, format %u", 
                     file_size, sample_rate, num_channels, bits_per_sample, audio_format);

            // Calculate audio duration
            size_t header_size = 44;
            if (audio_size > header_size) {
                size_t pcm_size = audio_size - header_size;
                double duration_seconds = (double)pcm_size / (sample_rate * num_channels * (bits_per_sample / 8));
                LOG_INFO("Audio duration: %.2f seconds", duration_seconds);
            }

            // Basic compatibility check
            int compatible = 1;
            if (num_channels != 1) {
                LOG_WARNING("Not mono audio (%u channels)", num_channels);
                compatible = 0;
            }
            if (bits_per_sample != 16) {
                LOG_WARNING("Not 16-bit audio (%u bits)", bits_per_sample);
                compatible = 0;
            }
            if (audio_format != 1) {
                LOG_WARNING("Not PCM format (%u)", audio_format);
                compatible = 0;
            }

            if (compatible) {
                LOG_INFO("WAV format is pipeline compatible");
            } else {
                LOG_WARNING("WAV format may need conversion");
            }
        }
    } else {
        LOG_WARNING("Data does not appear to be a standard WAV file");
        LOG_INFO("First 12 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 audio_data[0], audio_data[1], audio_data[2], audio_data[3],
                 audio_data[4], audio_data[5], audio_data[6], audio_data[7],
                 audio_data[8], audio_data[9], audio_data[10], audio_data[11]);
    }
}
