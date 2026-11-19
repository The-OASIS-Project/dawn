/*
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
 * Acoustic Echo Cancellation (AEC) Processor for DAWN
 *
 * This module wraps WebRTC's AEC3 algorithm to remove speaker echo
 * from microphone input, enabling barge-in during TTS playback.
 *
 * Thread Safety:
 * - aec_add_reference(): Safe to call from TTS thread (lock-free write to ring buffer)
 * - aec_process(): Safe to call from capture thread (per-frame locking)
 * - aec_init/cleanup(): Call from main thread only during startup/shutdown
 *
 * Real-Time Constraints:
 * - aec_process() uses per-frame locking (~480 samples = 10ms at 48kHz)
 * - No dynamic allocation in processing path
 * - Graceful degradation on errors (pass-through mode)
 *
 * Reference Buffer Behavior:
 * - Uses DAWN's ring_buffer_t which drops oldest data on overflow
 * - This is expected when TTS produces data faster than AEC consumes
 * - Does not affect echo cancellation quality (AEC only needs recent history)
 */

#ifndef AEC_PROCESSOR_H
#define AEC_PROCESSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile-time configuration validation
 *
 * AEC processes at 48kHz for optimal WebRTC AEC3 performance.
 * Audio capture should also be at 48kHz - downsampling to 16kHz
 * for ASR happens in the capture thread after AEC processing.
 */
#define AEC_SAMPLE_RATE 48000

/* Verify AEC sample rate matches capture rate at compile time */
#ifdef ENABLE_AEC
/* This will be checked in audio_capture_thread.c */
#endif

/**
 * @brief AEC processing constants
 *
 * WebRTC AEC3 processes in 10ms frames at 48kHz = 480 samples.
 * These values are fixed by the WebRTC API.
 */
#define AEC_FRAME_SAMPLES 480
#define AEC_FRAME_BYTES (AEC_FRAME_SAMPLES * sizeof(int16_t))

/**
 * @brief Maximum samples that can be processed in one call
 *
 * Limits memory allocation and prevents excessive lock hold times.
 * 24576 samples = 512ms at 48kHz, more than enough for any capture chunk.
 */
#define AEC_MAX_SAMPLES 24576

/**
 * @brief Consecutive error threshold before AEC disables itself
 */
#define AEC_MAX_CONSECUTIVE_ERRORS 10

/**
 * @brief Minimum reference buffer size in milliseconds
 *
 * Must accommodate: acoustic delay + system buffering + margin
 * Typical values: 100-200ms minimum, 500ms recommended
 */
#define AEC_MIN_REF_BUFFER_MS 100

/* Compile-time sanity checks */
#if AEC_MAX_SAMPLES < AEC_FRAME_SAMPLES
#error "AEC_MAX_SAMPLES must be >= AEC_FRAME_SAMPLES"
#endif

/**
 * @brief Noise suppression level enumeration
 */
typedef enum {
   AEC_NS_LEVEL_LOW = 0,      /**< Minimal noise suppression */
   AEC_NS_LEVEL_MODERATE = 1, /**< Balanced (default) */
   AEC_NS_LEVEL_HIGH = 2      /**< Aggressive noise suppression */
} aec_ns_level_t;

/**
 * @brief AEC runtime statistics for monitoring and debugging
 */
typedef struct {
   int estimated_delay_ms;         /**< Estimated acoustic delay */
   size_t ref_buffer_samples;      /**< Samples in reference buffer */
   int consecutive_errors;         /**< Error count (resets on success) */
   bool is_active;                 /**< True if AEC is processing */
   float avg_processing_time_us;   /**< Average processing time per frame */
   uint64_t frames_processed;      /**< Total frames successfully processed */
   uint64_t frames_passed_through; /**< Frames passed without AEC (no ref data) */
   float erle_db;                  /**< Echo Return Loss Enhancement in dB (higher = better) */
   float residual_echo_likelihood; /**< Probability of residual echo [0.0-1.0] */
   bool metrics_valid;             /**< True if ERLE/residual metrics are available */
} aec_stats_t;

/**
 * @brief AEC configuration options
 */
typedef struct {
   bool enable_noise_suppression;          /**< Enable NS (adds CPU load) */
   aec_ns_level_t noise_suppression_level; /**< NS aggressiveness */
   bool enable_high_pass_filter;           /**< Remove DC offset */
   bool mobile_mode;                       /**< Use AECM instead of AEC3 (lower CPU) */
   size_t ref_buffer_ms;                   /**< Reference buffer size in ms (default: 500) */
   uint16_t noise_gate_threshold; /**< Envelope gate threshold (0=disabled, 0-32767 range) */
   float gate_attack_ms;          /**< Gate attack time in ms (default: 2.0) */
   float gate_hold_ms;            /**< Gate hold time in ms (default: 50.0) */
   float gate_release_ms;         /**< Gate release time in ms (default: 100.0) */
   float gate_range_db;           /**< Attenuation when gate closed in dB (default: -40.0) */
   size_t acoustic_delay_ms;      /**< Delay from snd_pcm_writei to echo in mic (default: 70ms)
                                       Components: ALSA buffer (~50ms) + acoustic path (~20ms)
                                       Tune this per hardware if echo cancellation is poor */
} aec_config_t;

/**
 * @brief Get default AEC configuration
 *
 * Returns configuration with sensible defaults:
 * - Noise suppression: enabled, moderate level
 * - High-pass filter: enabled
 * - Mobile mode: disabled (full AEC3)
 * - Reference buffer: 500ms
 *
 * @return Configuration with sensible defaults for Jetson/RPi
 */
aec_config_t aec_get_default_config(void);

/**
 * @brief Initialize AEC processor with configuration
 *
 * Creates WebRTC AudioProcessing instance with AEC3 enabled.
 * Pre-allocates all buffers to avoid runtime allocation.
 *
 * Call AFTER TTS initialization (to ensure audio subsystem is ready).
 * TTS and AEC use separate resamplers - no shared resources.
 *
 * @param config Configuration options (NULL for defaults)
 * @return 0 on success, non-zero on error
 */
int aec_init(const aec_config_t *config);

/**
 * @brief Cleanup AEC processor
 *
 * Releases all AEC resources. Safe to call multiple times.
 * Blocks until any in-progress processing completes.
 *
 * Call BEFORE audio capture stops.
 */
void aec_cleanup(void);

/**
 * @brief Check if AEC is initialized and active
 *
 * Returns false if AEC failed initialization, hit error threshold,
 * or was never initialized.
 *
 * @return true if AEC is processing audio, false otherwise
 */
bool aec_is_enabled(void);

/**
 * @brief Add reference (far-end) audio from TTS playback
 *
 * Call this with TTS audio AFTER it's been written to the audio device.
 * Audio must be 16kHz mono S16_LE format - internally upsampled to 48kHz.
 *
 * This function is lock-free and safe to call from TTS thread.
 * Internally uses the thread-safe ring_buffer_write().
 *
 * @param samples Audio samples (16-bit signed, 16kHz)
 * @param num_samples Number of samples
 */
void aec_add_reference(const int16_t *samples, size_t num_samples);

/**
 * @brief Add reference audio with playback delay information
 *
 * Enhanced version that accepts the audio device's buffer delay.
 * This allows the AEC to accurately predict when audio will actually
 * play through the speaker, improving echo cancellation timing.
 *
 * Call this AFTER the playback write returns.
 * Query the delay using snd_pcm_delay() or pa_stream_get_latency().
 *
 * @param samples Audio samples (16-bit signed, 16kHz - internally upsampled)
 * @param num_samples Number of samples
 * @param playback_delay_us Delay in microseconds until audio plays through speaker
 *                          (from snd_pcm_delay or pa_stream_get_latency)
 */
void aec_add_reference_with_delay(const int16_t *samples,
                                  size_t num_samples,
                                  uint64_t playback_delay_us);

/**
 * @brief Process microphone audio to remove echo
 *
 * Takes raw microphone input at 48kHz and outputs echo-cancelled audio.
 * Audio must be 48kHz mono S16_LE format. The capture thread handles
 * downsampling to 16kHz for ASR after AEC processing.
 *
 * Uses per-frame locking (10ms granularity) to minimize impact
 * on real-time audio thread. On error, passes through unprocessed
 * audio to maintain audio continuity.
 *
 * If mic_in or clean_out is NULL, the output buffer is zeroed
 * to prevent undefined behavior in the caller.
 *
 * @param mic_in Input microphone samples at 48kHz (NULL safe - outputs silence)
 * @param clean_out Output buffer for echo-cancelled samples (same size as input)
 * @param num_samples Number of samples (must be <= AEC_MAX_SAMPLES)
 */
void aec_process(const int16_t *mic_in, int16_t *clean_out, size_t num_samples);

/**
 * @brief Get AEC runtime statistics
 *
 * @param stats Output structure for statistics
 * @return 0 on success, non-zero if AEC not initialized
 */
int aec_get_stats(aec_stats_t *stats);

/**
 * @brief Get current ERLE value for VAD gating decisions
 *
 * Returns the Echo Return Loss Enhancement in dB. Higher values indicate
 * better echo cancellation. Use this to gate VAD decisions:
 * - ERLE > 12dB: Good cancellation, trust VAD
 * - ERLE 6-12dB: Moderate cancellation, raise VAD threshold
 * - ERLE < 6dB: Poor cancellation, reject VAD during TTS
 *
 * @param erle_db Output ERLE value in dB (set to 0 if unavailable)
 * @return true if ERLE is valid, false if AEC not active or metrics unavailable
 */
bool aec_get_erle(float *erle_db);

/**
 * @brief Get residual echo likelihood for VAD gating
 *
 * @param likelihood Output probability [0.0-1.0], higher = more likely residual echo
 * @return true if metric is valid, false if unavailable
 */
bool aec_get_residual_echo_likelihood(float *likelihood);

/**
 * @brief Reset AEC state (clear buffers and error counters)
 *
 * Call this if audio routing changes or after long silence periods.
 * Re-enables AEC if it was disabled due to consecutive errors.
 *
 * Note: WebRTC AEC3 state reset support varies by version.
 * This function always clears the reference buffer and error counters.
 */
void aec_reset(void);

/**
 * @brief Update the acoustic delay hint for AEC processing
 *
 * Call this after measuring the actual speaker-to-microphone delay
 * (e.g., via calibration using boot greeting). The delay hint helps
 * the AEC align the reference signal with the echo in the mic input.
 *
 * The new delay takes effect immediately on subsequent frames.
 *
 * @param delay_ms Acoustic delay in milliseconds (0-500ms, clamped if out of range)
 */
void aec_set_delay_hint(int delay_ms);

/**
 * @brief Signal that TTS playback has stopped
 *
 * Call this when TTS playback completes normally or is interrupted.
 * This stops the underflow counting (which only matters during active playback)
 * and prepares the AEC for the next playback session.
 *
 * Thread-safe: can be called from TTS thread.
 */
void aec_signal_playback_stop(void);

// ============================================================================
// Audio Recording API for AEC Debugging
// ============================================================================

/**
 * @brief Set directory for recording output files
 *
 * @param dir Directory path (default: /tmp)
 */
void aec_set_recording_dir(const char *dir);

/**
 * @brief Enable or disable recording capability
 *
 * Must be called with true before aec_start_recording() will work.
 * When disabled with an active recording, stops the recording.
 *
 * @param enable true to enable, false to disable
 */
void aec_enable_recording(bool enable);

/**
 * @brief Check if recording is currently active
 *
 * @return true if actively recording, false otherwise
 */
bool aec_is_recording(void);

/**
 * @brief Check if recording capability is enabled
 *
 * @return true if recording is enabled, false otherwise
 */
bool aec_is_recording_enabled(void);

/**
 * @brief Start recording AEC audio streams
 *
 * Creates three WAV files with timestamped names:
 * - aec_mic_YYYYMMDD_HHMMSS.wav - Raw microphone input (48kHz)
 * - aec_ref_YYYYMMDD_HHMMSS.wav - TTS reference signal (48kHz)
 * - aec_out_YYYYMMDD_HHMMSS.wav - AEC output after processing (48kHz)
 *
 * Recording must be enabled first with aec_enable_recording(true).
 *
 * @return 0 on success, non-zero on error
 */
int aec_start_recording(void);

/**
 * @brief Stop recording and finalize WAV files
 *
 * Closes all recording files and updates WAV headers with final sizes.
 * Safe to call even if not recording.
 */
void aec_stop_recording(void);

#ifdef __cplusplus
}
#endif

#endif  // AEC_PROCESSOR_H
