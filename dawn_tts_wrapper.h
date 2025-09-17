#ifndef DAWN_TTS_WRAPPER_H
#define DAWN_TTS_WRAPPER_H

#include <stdint.h>
#include <stddef.h>

// ESP32 buffer limits
#define ESP32_SAMPLE_RATE 16000
#define ESP32_BITS_PER_SAMPLE 16  
#define ESP32_MAX_RECORD_TIME 30
#define ESP32_BUFFER_SAMPLES (ESP32_SAMPLE_RATE * ESP32_MAX_RECORD_TIME)
#define ESP32_MAX_RESPONSE_BYTES (ESP32_BUFFER_SAMPLES * sizeof(int16_t) + 1024)
#define SAFE_RESPONSE_LIMIT (ESP32_MAX_RESPONSE_BYTES - 1024)

// Error messages for TTS feedback
#define ERROR_MSG_LLM_TIMEOUT     "Sorry, the language model timed out. Please try again."
#define ERROR_MSG_TTS_FAILED      "Sorry, voice synthesis failed. Please try again."  
#define ERROR_MSG_SPEECH_FAILED   "Sorry, I could not understand your speech. Please try again."
#define ERROR_MSG_WAV_INVALID     "Sorry, invalid audio format received. Please try again."

// WAV Header Structure
typedef struct __attribute__((packed)) {
    char     riff_header[4];
    uint32_t wav_size;
    char     wave_header[4];
    char     fmt_header[4];
    uint32_t fmt_chunk_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_header[4];
    uint32_t data_bytes;
} WAVHeader;

#ifdef __cplusplus
extern "C" {
#endif

// === Piper TTS Integration ===

/**
 * Initialize Piper TTS system with specified model
 * @param model_path Path to .onnx model file (will auto-append .json for config)
 * @return 0 on success, -1 on error
 */
int dawn_tts_init(const char *model_path);

/**
 * Generate WAV audio from text using Piper TTS
 * Output is optimized for embedded device compatibility (16-bit/mono)
 * 
 * @param text Input text to synthesize
 * @param wav_data_out Pointer to receive allocated WAV data (caller must free)
 * @param wav_size_out Pointer to receive WAV data size in bytes
 * @return 0 on success, -1 on error
 */
int dawn_generate_tts_wav(const char *text, uint8_t **wav_data_out, size_t *wav_size_out);

/**
 * Cleanup Piper TTS resources
 */
void dawn_tts_cleanup(void);

/**
 * Check if TTS system is initialized and ready
 * @return 1 if initialized, 0 if not
 */
int dawn_tts_is_initialized(void);

// Truncate WAV file to fit within ESP32 buffer limits
int truncate_wav_response(const uint8_t *wav_data, size_t wav_size,
                         uint8_t **truncated_data_out, size_t *truncated_size_out);
int check_response_size_limit(size_t wav_size);
uint8_t* generate_error_tts(const char* error_message, size_t* tts_size_out);

#ifdef __cplusplus
}
#endif

#endif // DAWN_TTS_WRAPPER_H
