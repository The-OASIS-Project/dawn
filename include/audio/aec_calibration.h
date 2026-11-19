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
 * AEC Delay Calibration Module
 *
 * Measures the acoustic delay from speaker to microphone using cross-correlation
 * of TTS reference signal and microphone input during boot greeting playback.
 * This provides accurate delay hints to the AEC processor for optimal echo
 * cancellation performance.
 *
 * Usage:
 * 1. Call aec_cal_init() at startup with sample rate and max delay to search
 * 2. Call aec_cal_start() when TTS greeting playback begins
 * 3. Route reference samples through aec_cal_add_reference() during playback
 * 4. Route mic samples through aec_cal_add_mic() during capture
 * 5. Call aec_cal_finish() when playback ends to get measured delay
 * 6. Use measured delay to update AEC delay hint via aec_set_delay_hint()
 *
 * Thread Safety:
 * - aec_cal_add_reference(): Safe to call from TTS thread
 * - aec_cal_add_mic(): Safe to call from capture thread
 * - Other functions: Call from main thread only
 */

#ifndef AEC_CALIBRATION_H
#define AEC_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Calibration result codes
 */
#define AEC_CAL_SUCCESS 0
#define AEC_CAL_ERR_INVALID_PARAM 1
#define AEC_CAL_ERR_OUT_OF_MEMORY 2
#define AEC_CAL_ERR_NOT_ACTIVE 3
#define AEC_CAL_ERR_LOW_CORRELATION 4
#define AEC_CAL_ERR_AMBIGUOUS_PEAK 5
#define AEC_CAL_ERR_OUT_OF_RANGE 6
#define AEC_CAL_ERR_INSUFFICIENT_DATA 7
#define AEC_CAL_ERR_NO_MIC_DATA 8 /* no microphone audio (the mic wasn't there) */

/**
 * @brief Minimum correlation threshold for valid calibration
 *
 * Values below this indicate weak echo (muted speakers, headphones, etc.)
 */
#define AEC_CAL_MIN_CORRELATION 0.3f

/**
 * @brief Ambiguity threshold - secondary peak must be this much lower than primary
 */
#define AEC_CAL_AMBIGUITY_RATIO 0.7f

/**
 * @brief Minimum expected acoustic delay (milliseconds)
 *
 * Physical constraints make delays below this threshold impossible:
 * - Sound travels ~34cm per millisecond (343 m/s at 20°C)
 * - Hardware latency adds 5-20ms (ADC/DAC + buffers)
 * - Minimum speaker-to-mic distance is typically 10+ cm
 *
 * Searching below this threshold finds false peaks caused by:
 * - DC offset correlation
 * - Low-frequency noise present in both signals
 * - Electrical crosstalk
 *
 * 10ms minimum corresponds to ~3.4 meters or ~15ms hardware latency,
 * which is conservative for typical setups.
 */
#define AEC_CAL_MIN_DELAY_MS 10

/**
 * @brief Minimum greeting duration for reliable calibration (milliseconds)
 *
 * Greetings shorter than this may not provide enough audio content for
 * reliable cross-correlation. Typical boot greetings ("Hello",
 * "Good morning") are 0.5-1.5 seconds.
 */
#define AEC_CAL_MIN_GREETING_MS 500

/**
 * @brief Initialize calibration system
 *
 * Allocates buffers for reference and microphone samples.
 * Must be called before any other calibration functions.
 *
 * Buffer sizing:
 * - Reference buffer: ~2 seconds of audio at sample_rate
 * - Mic buffer: ~2 seconds + max_delay_ms margin
 *
 * Memory allocation note: This module uses dynamic allocation (malloc) rather
 * than static buffers because buffer sizes depend on runtime configuration
 * (sample_rate, max_delay_ms). Total allocation is ~400KB at 48kHz. Memory is
 * allocated once at init and freed at cleanup - no allocations in the
 * processing path.
 *
 * @param sample_rate Audio sample rate (e.g., 48000). Must be > 0.
 * @param max_delay_ms Maximum delay to search for (e.g., 200). Must be > 0
 *        and <= 500ms (buffer constraint).
 * @return AEC_CAL_SUCCESS on success, error code on failure
 */
int aec_cal_init(int sample_rate, int max_delay_ms);

/**
 * @brief Start calibration capture
 *
 * Clears buffers and begins collecting reference and mic samples.
 * Call this when TTS greeting playback begins.
 *
 * Safe to call even if not initialized (will be a no-op).
 */
void aec_cal_start(void);

/**
 * @brief Add reference samples during calibration
 *
 * Call this from the TTS reference path (aec_add_reference) during
 * calibration. Samples are appended to the internal reference buffer.
 *
 * Thread-safe: can be called from TTS thread while capture thread
 * calls aec_cal_add_mic().
 *
 * @param samples Audio samples (16-bit signed, at configured sample rate)
 * @param num_samples Number of samples to add
 */
void aec_cal_add_reference(const int16_t *samples, size_t num_samples);

/**
 * @brief Add microphone samples during calibration
 *
 * Call this from the AEC mic processing path (aec_process) during
 * calibration. Samples are appended to the internal mic buffer.
 *
 * Thread-safe: can be called from capture thread while TTS thread
 * calls aec_cal_add_reference().
 *
 * @param samples Audio samples (16-bit signed, at configured sample rate)
 * @param num_samples Number of samples to add
 */
void aec_cal_add_mic(const int16_t *samples, size_t num_samples);

/**
 * @brief Stop calibration and compute delay
 *
 * Performs cross-correlation between reference and mic buffers to find
 * the acoustic delay. Call this when TTS greeting playback completes.
 *
 * The correlation search finds the lag that maximizes:
 *   correlation[d] = sum(ref[i] * mic[i + d]) / sqrt(sum(ref^2) * sum(mic^2))
 *
 * @param delay_ms Output: measured delay in milliseconds (valid only on success)
 * @return AEC_CAL_SUCCESS on success, error code on failure:
 *         - AEC_CAL_ERR_NOT_ACTIVE: Calibration not running
 *         - AEC_CAL_ERR_LOW_CORRELATION: Weak echo (< 0.3 peak)
 *         - AEC_CAL_ERR_AMBIGUOUS_PEAK: Multiple similar peaks
 *         - AEC_CAL_ERR_OUT_OF_RANGE: Delay outside expected range
 *         - AEC_CAL_ERR_INSUFFICIENT_DATA: Not enough samples captured
 */
int aec_cal_finish(int *delay_ms);

/**
 * @brief Check if calibration is currently in progress
 *
 * @return true if calibration is active (between start and finish), false otherwise
 */
bool aec_cal_is_active(void);

/**
 * @brief Check if calibration system is initialized
 *
 * @return true if aec_cal_init() was called successfully, false otherwise
 */
bool aec_cal_is_initialized(void);

/**
 * @brief Get the last measured correlation peak value
 *
 * Useful for debugging and confidence assessment.
 *
 * @return Peak correlation value [0.0-1.0] from last aec_cal_finish() call,
 *         or 0.0 if no calibration has been performed
 */
float aec_cal_get_last_correlation(void);

/**
 * @brief Cleanup calibration resources
 *
 * Frees all allocated buffers. Safe to call multiple times or if not initialized.
 */
void aec_cal_cleanup(void);

/**
 * @brief Request calibration on next TTS playback
 *
 * Sets a flag indicating calibration should start when the next TTS
 * playback begins. This decouples the calibration request from the
 * TTS module - callers request calibration, TTS just checks the flag.
 *
 * Thread-safe: can be called from any thread.
 */
void aec_cal_set_pending(void);

/**
 * @brief Check if calibration is pending and atomically clear the flag
 *
 * Used by TTS playback start callback to check if it should begin
 * calibration capture. If pending, clears the flag and returns true.
 *
 * Thread-safe: uses atomic exchange to ensure only one caller "wins".
 *
 * @return true if calibration was pending (and is now cleared), false otherwise
 */
bool aec_cal_check_and_clear_pending(void);

/**
 * @brief Check if calibration request is pending
 *
 * Non-consuming check - does not clear the pending flag.
 *
 * @return true if calibration is pending, false otherwise
 */
bool aec_cal_is_pending(void);

#ifdef __cplusplus
}
#endif

#endif  // AEC_CALIBRATION_H
