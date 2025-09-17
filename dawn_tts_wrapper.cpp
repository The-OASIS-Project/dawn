#include "dawn_tts_wrapper.h"
#include "piper.hpp"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sstream>
#include <exception>
#include <memory>

// Piper TTS Integration Implementation

// Static TTS state (singleton pattern for C interface)
static piper::PiperConfig tts_config;
static piper::Voice tts_voice;
static bool tts_initialized = false;

extern "C" {

int dawn_tts_init(const char *model_path) {
    if (tts_initialized) {
        LOG_INFO("TTS already initialized");
        return 0;
    }

    if (!model_path) {
        LOG_ERROR("No model path provided");
        return -1;
    }

    try {
        LOG_INFO("Initializing Piper TTS");
        LOG_INFO("Model path: %s", model_path);

        // Initialize Piper configuration
        tts_config.useESpeak = true;
        tts_config.eSpeakDataPath = "";  // Use default

        // Initialize Piper
        piper::initialize(tts_config);

        // Prepare model paths
        std::string modelPath = model_path;
        std::string configPath = modelPath + ".json";
        std::optional<piper::SpeakerId> speakerIdOpt;

        LOG_INFO("Loading voice model: %s", modelPath.c_str());
        LOG_INFO("Loading voice config: %s", configPath.c_str());

        // Load voice model
        piper::loadVoice(tts_config, modelPath, configPath, tts_voice, 
                        speakerIdOpt, false);

        // Log final configuration
        LOG_INFO("TTS Configuration:");
        LOG_INFO("  Sample rate: %d Hz", tts_voice.synthesisConfig.sampleRate);
        LOG_INFO("  Sample width: %d bytes (%d-bit)", 
               tts_voice.synthesisConfig.sampleWidth, 
               tts_voice.synthesisConfig.sampleWidth * 8);
        LOG_INFO("  Channels: %d", tts_voice.synthesisConfig.channels);
        LOG_INFO("  Number of speakers: %d", tts_voice.modelConfig.numSpeakers);

        if (tts_voice.synthesisConfig.speakerId) {
            LOG_INFO("  Speaker ID: %ld", tts_voice.synthesisConfig.speakerId.value());
        } else {
            LOG_INFO("  Speaker ID: default");
        }

        LOG_INFO("ESP32 will resample %d Hz to 48 kHz for I2S playback", 
                 tts_voice.synthesisConfig.sampleRate);

        tts_initialized = true;
        LOG_INFO("Piper TTS initialized successfully");

        return 0;

    } catch (const std::exception& e) {
        LOG_ERROR("TTS initialization failed: %s", e.what());
        tts_initialized = false;
        return -1;
    } catch (...) {
        LOG_ERROR("Unknown TTS initialization error");
        tts_initialized = false;
        return -1;
    }
}

// Check if TTS WAV response fits within ESP32 buffer limits
int check_response_size_limit(size_t wav_size) {
    LOG_INFO("Response size: %zu bytes (limit: %ld bytes)", wav_size, SAFE_RESPONSE_LIMIT);

    if (wav_size <= SAFE_RESPONSE_LIMIT) {
        LOG_INFO("Response fits within ESP32 buffer limits");
        return 1;
    } else {
        LOG_WARNING("Response exceeds ESP32 buffer limits by %zu bytes", 
                   wav_size - SAFE_RESPONSE_LIMIT);
        return 0;
    }
}

// Truncate WAV file to fit within ESP32 buffer limits
int truncate_wav_response(const uint8_t *wav_data, size_t wav_size,
                         uint8_t **truncated_data_out, size_t *truncated_size_out) {

    if (!wav_data || wav_size < 44 || !truncated_data_out || !truncated_size_out) {
        LOG_ERROR("Invalid parameters for WAV truncation");
        return -1;
    }

    LOG_INFO("Truncating WAV from %zu to %ld bytes", wav_size, SAFE_RESPONSE_LIMIT);

    // Parse original WAV header
    const WAVHeader *original_header = (const WAVHeader *)wav_data;

    // Calculate how much audio data we can keep
    size_t header_size = sizeof(WAVHeader);
    size_t max_audio_data = SAFE_RESPONSE_LIMIT - header_size;
    size_t original_audio_data = wav_size - header_size;

    if (original_audio_data <= max_audio_data) {
        LOG_INFO("No truncation needed");
        return -1;
    }

    // Ensure we truncate on sample boundaries (2 bytes per sample for 16-bit mono)
    max_audio_data = (max_audio_data / 2) * 2;
    size_t truncated_total_size = header_size + max_audio_data;

    // Calculate duration change
    uint32_t sample_rate = le32toh(original_header->sample_rate);
    double original_duration = (double)original_audio_data / (sample_rate * 2);
    double truncated_duration = (double)max_audio_data / (sample_rate * 2);

    LOG_INFO("Duration: %.2f -> %.2f seconds", original_duration, truncated_duration);

    // Allocate new buffer
    uint8_t *truncated_data = (uint8_t *)malloc(truncated_total_size);
    if (!truncated_data) {
        LOG_ERROR("Failed to allocate truncated buffer");
        return -1;
    }

    // Copy and modify header
    WAVHeader *new_header = (WAVHeader *)truncated_data;
    memcpy(new_header, original_header, header_size);

    // Update header with new sizes
    new_header->wav_size = htole32(truncated_total_size - 8);
    new_header->data_bytes = htole32(max_audio_data);

    // Copy truncated audio data
    memcpy(truncated_data + header_size, wav_data + header_size, max_audio_data);

    *truncated_data_out = truncated_data;
    *truncated_size_out = truncated_total_size;

    LOG_INFO("WAV truncation complete: %zu bytes", truncated_total_size);
    return 0;
}

int dawn_generate_tts_wav(const char *text, uint8_t **wav_data_out, size_t *wav_size_out) {
    if (!tts_initialized) {
        LOG_ERROR("TTS not initialized");
        return -1;
    }

    if (!text || !wav_data_out || !wav_size_out) {
        LOG_ERROR("Invalid parameters for TTS generation");
        return -1;
    }

    if (strlen(text) == 0) {
        LOG_ERROR("Empty text provided for TTS");
        return -1;
    }

    try {
        LOG_INFO("Generating TTS for: \"%s\"", text);
        LOG_INFO("Text length: %zu characters", strlen(text));

        // Create output stream for WAV data
        std::ostringstream audioStream;
        piper::SynthesisResult result;

        LOG_INFO("Generating audio...");

        // Generate WAV in memory
        piper::textToWavFile(tts_config, tts_voice, std::string(text), 
                            audioStream, result);

        LOG_INFO("Generation complete:");
        LOG_INFO("  Audio duration: %.2f seconds", result.audioSeconds);
        LOG_INFO("  Inference time: %.2f seconds", result.inferSeconds);
        LOG_INFO("  Real-time factor: %.2fx", result.realTimeFactor);

        // Get WAV data from stream
        std::string wavData = audioStream.str();

        if (wavData.empty()) {
            LOG_ERROR("Generated WAV data is empty");
            return -1;
        }

        // Allocate C-compatible buffer
        *wav_size_out = wavData.size();
        *wav_data_out = (uint8_t*)malloc(*wav_size_out);

        if (!*wav_data_out) {
            LOG_ERROR("Failed to allocate %zu bytes for WAV data", *wav_size_out);
            return -1;
        }

        // Copy WAV data to C buffer
        memcpy(*wav_data_out, wavData.data(), *wav_size_out);

        LOG_INFO("TTS generation successful:");
        LOG_INFO("  Generated WAV size: %zu bytes", *wav_size_out);
        LOG_INFO("  Audio duration: %.2f seconds", result.audioSeconds);
        LOG_INFO("  Real-time factor: %.2fx", result.realTimeFactor);

        return 0;

    } catch (const std::exception& e) {
        LOG_ERROR("TTS generation failed: %s", e.what());
        return -1;
    } catch (...) {
        LOG_ERROR("Unknown TTS generation error");
        return -1;
    }
}

uint8_t* generate_error_tts(const char* error_message, size_t* tts_size_out) {
    uint8_t *tts_wav_data = NULL;
    size_t tts_wav_size = 0;

    LOG_INFO("Generating error TTS: \"%s\"", error_message);

    int tts_result = dawn_generate_tts_wav(error_message, &tts_wav_data, &tts_wav_size);

    if (tts_result == 0 && tts_wav_data && tts_wav_size > 0) {
        // Apply same size/truncation logic as normal TTS
        if (check_response_size_limit(tts_wav_size)) {
            *tts_size_out = tts_wav_size;
            return tts_wav_data;
        } else {
            uint8_t *truncated_data = NULL;
            size_t truncated_size = 0;

            if (truncate_wav_response(tts_wav_data, tts_wav_size,
                                    &truncated_data, &truncated_size) == 0) {
                free(tts_wav_data);
                *tts_size_out = truncated_size;
                return truncated_data;
            }
            free(tts_wav_data);
        }
    }

    LOG_ERROR("Failed to generate error TTS");
    *tts_size_out = 0;
    return NULL;
}

void dawn_tts_cleanup(void) {
    if (!tts_initialized) {
        return;
    }

    LOG_INFO("Cleaning up Piper TTS resources");

    try {
        piper::terminate(tts_config);
        tts_initialized = false;
        LOG_INFO("TTS cleanup complete");
    } catch (const std::exception& e) {
        LOG_WARNING("TTS cleanup error: %s", e.what());
    } catch (...) {
        LOG_WARNING("Unknown TTS cleanup error");
    }
}

int dawn_tts_is_initialized(void) {
    return tts_initialized ? 1 : 0;
}

} // extern "C"
