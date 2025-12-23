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
 */

/**
 * @file metrics.h
 * @brief TUI metrics collection infrastructure for DAWN
 *
 * Provides thread-safe metrics collection for real-time monitoring.
 * All timing values are in milliseconds unless otherwise noted.
 */

#ifndef METRICS_H
#define METRICS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "llm/llm_interface.h"
#include "state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of activity log entries in circular buffer */
#define METRICS_MAX_LOG_ENTRIES 100

/** Maximum length of each activity log entry */
#define METRICS_MAX_LOG_LENGTH 256

/** Number of states for time tracking */
#define METRICS_NUM_STATES DAWN_STATE_INVALID

/**
 * @brief Centralized metrics structure for TUI display
 *
 * Thread-safe structure containing all metrics for DAWN monitoring.
 * Access through metrics_* functions, not directly.
 */
typedef struct {
   /* Session statistics */
   uint32_t queries_total;   /**< Total number of queries processed */
   uint32_t queries_cloud;   /**< Queries processed via cloud LLM */
   uint32_t queries_local;   /**< Queries processed via local LLM */
   uint32_t errors_count;    /**< Total errors encountered */
   uint32_t fallbacks_count; /**< Cloud to local fallback count */

   /* Token counters (cumulative for session) */
   uint64_t tokens_cloud_input;  /**< Cloud LLM input tokens */
   uint64_t tokens_cloud_output; /**< Cloud LLM output tokens */
   uint64_t tokens_local_input;  /**< Local LLM input tokens */
   uint64_t tokens_local_output; /**< Local LLM output tokens */
   uint64_t tokens_cached;       /**< Cached tokens (prompt caching) */

   /* Last query timing (milliseconds) */
   double last_vad_time_ms;       /**< Last VAD detection time */
   double last_asr_time_ms;       /**< Last ASR processing time */
   double last_asr_rtf;           /**< Last ASR Real-Time Factor */
   double last_llm_ttft_ms;       /**< Last LLM Time To First Token */
   double last_llm_total_ms;      /**< Last LLM total processing time */
   double last_tts_time_ms;       /**< Last TTS generation time */
   double last_total_pipeline_ms; /**< Last total pipeline latency */

   /* Session averages (rolling) */
   double avg_vad_ms;            /**< Average VAD time */
   double avg_asr_ms;            /**< Average ASR time */
   double avg_asr_rtf;           /**< Average ASR RTF */
   double avg_llm_ttft_ms;       /**< Average LLM TTFT */
   double avg_llm_total_ms;      /**< Average LLM total time */
   double avg_tts_ms;            /**< Average TTS time */
   double avg_total_pipeline_ms; /**< Average pipeline latency */

   /* Running totals for average calculation */
   uint32_t vad_count;      /**< Number of VAD measurements */
   uint32_t asr_count;      /**< Number of ASR measurements */
   uint32_t llm_count;      /**< Number of LLM measurements */
   uint32_t tts_count;      /**< Number of TTS measurements */
   uint32_t pipeline_count; /**< Number of pipeline measurements */

   /* Real-time state */
   float current_vad_probability;           /**< Current VAD speech probability (0.0-1.0) */
   dawn_state_t current_state;              /**< Current state machine state */
   llm_type_t current_llm_type;             /**< Current LLM type (local/cloud) */
   cloud_provider_t current_cloud_provider; /**< Current cloud provider */

   /* AEC status and calibration */
   bool aec_enabled;      /**< Is AEC compiled and active? */
   bool aec_calibrated;   /**< Was boot calibration successful? */
   int aec_delay_ms;      /**< Measured acoustic delay (0 if uncalibrated) */
   float aec_correlation; /**< Calibration correlation quality (0.0-1.0) */

   /* Search summarizer stats */
   char summarizer_backend[16];       /**< Backend name ("disabled", "local", "default") */
   size_t summarizer_threshold;       /**< Current threshold in bytes */
   uint32_t summarizer_call_count;    /**< Total summarizations performed */
   size_t summarizer_total_in_bytes;  /**< Total input bytes processed */
   size_t summarizer_total_out_bytes; /**< Total output bytes produced */
   size_t summarizer_last_in_bytes;   /**< Last input size */
   size_t summarizer_last_out_bytes;  /**< Last output size */

   /* Audio status */
   float audio_buffer_fill_pct;                /**< Ring buffer fill percentage (0-100) */
   uint32_t bargein_count;                     /**< Times user interrupted TTS */
   char last_asr_text[METRICS_MAX_LOG_LENGTH]; /**< Last ASR result (even without wake word) */
   double last_asr_text_time_ms;               /**< When last ASR text was captured */

   /* Last command/response text for display */
   char last_user_command[METRICS_MAX_LOG_LENGTH]; /**< Last user command text */
   char last_ai_response[METRICS_MAX_LOG_LENGTH];  /**< Last AI response text */

   /* State time tracking (seconds spent in each state) */
   time_t state_time[METRICS_NUM_STATES]; /**< Time spent in each state */
   time_t state_entry_time;               /**< When current state was entered */

   /* Activity log circular buffer */
   char activity_log[METRICS_MAX_LOG_ENTRIES][METRICS_MAX_LOG_LENGTH];
   int log_head;  /**< Next write position */
   int log_count; /**< Number of entries (max METRICS_MAX_LOG_ENTRIES) */

   /* Session timing */
   time_t session_start_time; /**< When session started */

   /* Thread safety */
   pthread_mutex_t mutex; /**< Mutex for thread-safe access */
} dawn_metrics_t;

/* ============================================================================
 * Initialization and Cleanup
 * ============================================================================ */

/**
 * @brief Initialize the metrics system
 *
 * Must be called before any other metrics functions.
 * Safe to call multiple times (idempotent).
 *
 * @return 0 on success, non-zero on failure
 */
int metrics_init(void);

/**
 * @brief Cleanup the metrics system
 *
 * Releases resources. Should be called on application exit.
 */
void metrics_cleanup(void);

/**
 * @brief Reset all session statistics
 *
 * Clears counters, averages, and activity log.
 * Does not reset session start time.
 */
void metrics_reset(void);

/* ============================================================================
 * State Tracking
 * ============================================================================ */

/**
 * @brief Update current state machine state
 *
 * Records state transition and updates time spent in previous state.
 *
 * @param new_state New state to transition to
 */
void metrics_update_state(dawn_state_t new_state);

/**
 * @brief Update current VAD speech probability
 *
 * Called on every VAD inference (~every 50ms).
 *
 * @param probability Speech probability (0.0-1.0)
 */
void metrics_update_vad_probability(float probability);

/* ============================================================================
 * AEC Status
 * ============================================================================ */

/**
 * @brief Update AEC enabled status
 *
 * Called at startup to indicate if AEC is compiled and active.
 *
 * @param enabled true if AEC is enabled, false otherwise
 */
void metrics_update_aec_enabled(bool enabled);

/**
 * @brief Record AEC calibration result
 *
 * Called after boot calibration completes.
 *
 * @param success true if calibration succeeded
 * @param delay_ms Measured acoustic delay in milliseconds
 * @param correlation Peak correlation value (0.0-1.0)
 */
void metrics_record_aec_calibration(bool success, int delay_ms, float correlation);

/* ============================================================================
 * Search Summarizer Status
 * ============================================================================ */

/**
 * @brief Set summarizer configuration for display
 *
 * @param backend Backend name ("disabled", "local", "default")
 * @param threshold Threshold in bytes
 */
void metrics_set_summarizer_config(const char *backend, size_t threshold);

/**
 * @brief Record a summarization operation
 *
 * @param input_bytes Input size before summarization
 * @param output_bytes Output size after summarization
 */
void metrics_record_summarization(size_t input_bytes, size_t output_bytes);

/* ============================================================================
 * Audio Status
 * ============================================================================ */

/**
 * @brief Update audio ring buffer fill percentage
 *
 * @param fill_pct Buffer fill percentage (0.0-100.0)
 */
void metrics_update_audio_buffer_fill(float fill_pct);

/**
 * @brief Record a barge-in event (user interrupted TTS)
 */
void metrics_record_bargein(void);

/**
 * @brief Set last ASR text result (even without wake word)
 *
 * Used to show what was heard in real-time display.
 *
 * @param text ASR result text
 * @param processing_time_ms ASR processing time
 */
void metrics_set_last_asr_text(const char *text, double processing_time_ms);

/* ============================================================================
 * ASR Timing
 * ============================================================================ */

/**
 * @brief Record ASR completion timing
 *
 * @param time_ms Processing time in milliseconds
 * @param rtf Real-Time Factor (processing_time / audio_duration)
 */
void metrics_record_asr_timing(double time_ms, double rtf);

/* ============================================================================
 * LLM Timing and Tokens
 * ============================================================================ */

/**
 * @brief Record LLM Time To First Token
 *
 * Called when first chunk received from streaming LLM.
 *
 * @param ttft_ms Time from request to first token in milliseconds
 */
void metrics_record_llm_ttft(double ttft_ms);

/**
 * @brief Record LLM total completion time
 *
 * Called when LLM response is complete.
 *
 * @param total_ms Total time from request to completion in milliseconds
 */
void metrics_record_llm_total_time(double total_ms);

/**
 * @brief Record LLM token usage and update last-used provider
 *
 * Also updates the "current" LLM configuration in metrics to reflect
 * the last used provider. This ensures stats export shows accurate
 * provider info even with per-session LLM configurations.
 *
 * @param type LLM_LOCAL or LLM_CLOUD
 * @param cloud_provider Cloud provider (only used if type is LLM_CLOUD)
 * @param input_tokens Number of input tokens
 * @param output_tokens Number of output tokens
 * @param cached_tokens Number of cached tokens (0 if not applicable)
 */
void metrics_record_llm_tokens(llm_type_t type,
                               cloud_provider_t cloud_provider,
                               int input_tokens,
                               int output_tokens,
                               int cached_tokens);

/**
 * @brief Record LLM query completion
 *
 * Increments query counter for the appropriate LLM type.
 *
 * @param type LLM_LOCAL or LLM_CLOUD
 */
void metrics_record_llm_query(llm_type_t type);

/**
 * @brief Record a fallback from cloud to local LLM
 */
void metrics_record_fallback(void);

/**
 * @brief Record an error
 */
void metrics_record_error(void);

/**
 * @brief Update current LLM configuration
 *
 * @param type LLM_LOCAL or LLM_CLOUD
 * @param provider Cloud provider (if type is LLM_CLOUD)
 */
void metrics_update_llm_config(llm_type_t type, cloud_provider_t provider);

/* ============================================================================
 * TTS Timing
 * ============================================================================ */

/**
 * @brief Record TTS generation timing
 *
 * @param time_ms TTS generation time in milliseconds
 */
void metrics_record_tts_timing(double time_ms);

/* ============================================================================
 * Pipeline Timing
 * ============================================================================ */

/**
 * @brief Record total pipeline latency
 *
 * Called after complete query processing (ASR + LLM + TTS).
 *
 * @param total_ms Total pipeline time in milliseconds
 */
void metrics_record_pipeline_total(double total_ms);

/* ============================================================================
 * Activity Log
 * ============================================================================ */

/**
 * @brief Add entry to activity log
 *
 * Supports printf-style formatting. Automatically timestamps entries.
 *
 * @param format printf format string
 * @param ... Format arguments
 */
void metrics_log_activity(const char *format, ...);

/**
 * @brief Record user command for display
 *
 * @param command User command text
 */
void metrics_set_last_user_command(const char *command);

/**
 * @brief Record AI response for display
 *
 * @param response AI response text
 */
void metrics_set_last_ai_response(const char *response);

/* ============================================================================
 * Thread-Safe Snapshot
 * ============================================================================ */

/**
 * @brief Get thread-safe copy of current metrics
 *
 * Copies all metrics data while holding the mutex.
 * The returned snapshot does NOT include a valid mutex (do not use for locking).
 *
 * @param snapshot Pointer to structure to fill with current metrics
 */
void metrics_get_snapshot(dawn_metrics_t *snapshot);

/**
 * @brief Get session uptime in seconds
 *
 * @return Seconds since session started
 */
time_t metrics_get_uptime(void);

/* ============================================================================
 * JSON Export
 * ============================================================================ */

/**
 * @brief Export metrics to JSON file
 *
 * Writes session statistics to JSON file for analysis.
 *
 * @param filepath Path to output file (created or overwritten)
 * @return 0 on success, non-zero on failure
 */
int metrics_export_json(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* METRICS_H */
