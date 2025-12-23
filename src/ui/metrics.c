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
 * @file metrics.c
 * @brief Implementation of TUI metrics collection infrastructure
 */

#include "ui/metrics.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "logging.h"

/* Global metrics instance */
static dawn_metrics_t g_metrics;
static int g_initialized = 0;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * @brief Sanitize string for TUI display
 *
 * Replaces multi-byte UTF-8 characters with ASCII equivalents where possible,
 * and removes/replaces non-printable characters.
 */
static void sanitize_for_display(char *str) {
   if (str == NULL)
      return;

   unsigned char *src = (unsigned char *)str;
   unsigned char *dst = (unsigned char *)str;

   while (*src) {
      /* Check for multi-byte UTF-8 sequences */
      if ((src[0] & 0xE0) == 0xC0 && (src[1] & 0xC0) == 0x80) {
         /* 2-byte UTF-8: common accented characters, skip and replace with base */
         src += 2;
         /* Skip entirely - most are accents we don't need */
      } else if ((src[0] & 0xF0) == 0xE0 && (src[1] & 0xC0) == 0x80 && (src[2] & 0xC0) == 0x80) {
         /* 3-byte UTF-8: includes smart quotes, em-dashes, etc. */
         /* Check for common replaceable characters */
         if (src[0] == 0xE2 && src[1] == 0x80) {
            /* U+2018-U+201F: various quotation marks */
            if (src[2] == 0x98 || src[2] == 0x99) {
               *dst++ = '\''; /* ' or ' → ' */
            } else if (src[2] == 0x9C || src[2] == 0x9D) {
               *dst++ = '"'; /* " or " → " */
            } else if (src[2] == 0x93 || src[2] == 0x94) {
               *dst++ = '-'; /* en-dash or em-dash → - */
            } else if (src[2] == 0xA6) {
               *dst++ = '.';
               *dst++ = '.';
               *dst++ = '.'; /* … → ... */
            } else {
               *dst++ = ' '; /* Other U+20xx → space */
            }
         } else {
            *dst++ = ' '; /* Other 3-byte → space */
         }
         src += 3;
      } else if ((src[0] & 0xF8) == 0xF0 && (src[1] & 0xC0) == 0x80 && (src[2] & 0xC0) == 0x80 &&
                 (src[3] & 0xC0) == 0x80) {
         /* 4-byte UTF-8: emoji and rare chars → skip */
         src += 4;
      } else if (*src >= 32 && *src <= 126) {
         /* Printable ASCII: keep as-is */
         *dst++ = *src++;
      } else if (*src == '\t' || *src == '\n' || *src == '\r') {
         /* Whitespace → single space */
         *dst++ = ' ';
         src++;
      } else {
         /* Other non-printable → skip */
         src++;
      }
   }
   *dst = '\0';
}

/**
 * @brief Update rolling average
 *
 * Calculates new average given previous average, count, and new value.
 */
static double update_average(double old_avg, uint32_t count, double new_value) {
   if (count == 0) {
      return new_value;
   }
   return old_avg + (new_value - old_avg) / (count + 1);
}

/**
 * @brief Get current timestamp string for activity log
 */
static void get_timestamp_str(char *buf, size_t buf_size) {
   time_t now = time(NULL);
   struct tm *tm_info = localtime(&now);
   strftime(buf, buf_size, "%H:%M:%S", tm_info);
}

/* ============================================================================
 * Initialization and Cleanup
 * ============================================================================ */

int metrics_init(void) {
   if (g_initialized) {
      return 0; /* Already initialized */
   }

   memset(&g_metrics, 0, sizeof(dawn_metrics_t));

   if (pthread_mutex_init(&g_metrics.mutex, NULL) != 0) {
      LOG_ERROR("Failed to initialize metrics mutex");
      return 1;
   }

   g_metrics.session_start_time = time(NULL);
   g_metrics.state_entry_time = g_metrics.session_start_time;
   g_metrics.current_state = DAWN_STATE_SILENCE;
   g_metrics.current_llm_type = LLM_UNDEFINED;
   g_metrics.current_cloud_provider = CLOUD_PROVIDER_NONE;

   g_initialized = 1;
   LOG_INFO("Metrics system initialized");

   return 0;
}

void metrics_cleanup(void) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_destroy(&g_metrics.mutex);
   g_initialized = 0;
   LOG_INFO("Metrics system cleaned up");
}

void metrics_reset(void) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   /* Preserve session start time and current config */
   time_t start_time = g_metrics.session_start_time;
   dawn_state_t current_state = g_metrics.current_state;
   llm_type_t llm_type = g_metrics.current_llm_type;
   cloud_provider_t cloud_provider = g_metrics.current_cloud_provider;

   /* Clear all counters and averages */
   g_metrics.queries_total = 0;
   g_metrics.queries_cloud = 0;
   g_metrics.queries_local = 0;
   g_metrics.errors_count = 0;
   g_metrics.fallbacks_count = 0;

   g_metrics.tokens_cloud_input = 0;
   g_metrics.tokens_cloud_output = 0;
   g_metrics.tokens_local_input = 0;
   g_metrics.tokens_local_output = 0;
   g_metrics.tokens_cached = 0;

   g_metrics.last_vad_time_ms = 0;
   g_metrics.last_asr_time_ms = 0;
   g_metrics.last_asr_rtf = 0;
   g_metrics.last_llm_ttft_ms = 0;
   g_metrics.last_llm_total_ms = 0;
   g_metrics.last_tts_time_ms = 0;
   g_metrics.last_total_pipeline_ms = 0;

   g_metrics.avg_vad_ms = 0;
   g_metrics.avg_asr_ms = 0;
   g_metrics.avg_asr_rtf = 0;
   g_metrics.avg_llm_ttft_ms = 0;
   g_metrics.avg_llm_total_ms = 0;
   g_metrics.avg_tts_ms = 0;
   g_metrics.avg_total_pipeline_ms = 0;

   g_metrics.vad_count = 0;
   g_metrics.asr_count = 0;
   g_metrics.llm_count = 0;
   g_metrics.tts_count = 0;
   g_metrics.pipeline_count = 0;

   memset(g_metrics.state_time, 0, sizeof(g_metrics.state_time));
   memset(g_metrics.activity_log, 0, sizeof(g_metrics.activity_log));
   g_metrics.log_head = 0;
   g_metrics.log_count = 0;

   memset(g_metrics.last_user_command, 0, sizeof(g_metrics.last_user_command));
   memset(g_metrics.last_ai_response, 0, sizeof(g_metrics.last_ai_response));
   memset(g_metrics.last_asr_text, 0, sizeof(g_metrics.last_asr_text));
   g_metrics.last_asr_text_time_ms = 0;

   /* Reset audio status counters (but preserve AEC calibration - it's session-persistent) */
   g_metrics.audio_buffer_fill_pct = 0;
   g_metrics.bargein_count = 0;

   /* Restore preserved values */
   g_metrics.session_start_time = start_time;
   g_metrics.state_entry_time = time(NULL);
   g_metrics.current_state = current_state;
   g_metrics.current_llm_type = llm_type;
   g_metrics.current_cloud_provider = cloud_provider;

   pthread_mutex_unlock(&g_metrics.mutex);

   LOG_INFO("Metrics reset");
}

/* ============================================================================
 * State Tracking
 * ============================================================================ */

void metrics_update_state(dawn_state_t new_state) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   time_t now = time(NULL);
   dawn_state_t old_state = g_metrics.current_state;

   /* Update time spent in previous state */
   if (old_state < METRICS_NUM_STATES) {
      g_metrics.state_time[old_state] += (now - g_metrics.state_entry_time);
   }

   /* Transition to new state */
   g_metrics.current_state = new_state;
   g_metrics.state_entry_time = now;

   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_update_vad_probability(float probability) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.current_vad_probability = probability;
   pthread_mutex_unlock(&g_metrics.mutex);
}

/* ============================================================================
 * AEC Status
 * ============================================================================ */

void metrics_update_aec_enabled(bool enabled) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.aec_enabled = enabled;
   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_record_aec_calibration(bool success, int delay_ms, float correlation) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.aec_calibrated = success;
   g_metrics.aec_delay_ms = delay_ms;
   g_metrics.aec_correlation = correlation;
   pthread_mutex_unlock(&g_metrics.mutex);

   if (success) {
      metrics_log_activity("AEC calibration: %dms delay (corr: %.2f)", delay_ms, correlation);
   } else {
      metrics_log_activity("AEC calibration failed");
   }
}

/* ============================================================================
 * Search Summarizer Status
 * ============================================================================ */

void metrics_set_summarizer_config(const char *backend, size_t threshold) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   if (backend) {
      snprintf(g_metrics.summarizer_backend, sizeof(g_metrics.summarizer_backend), "%s", backend);
   } else {
      snprintf(g_metrics.summarizer_backend, sizeof(g_metrics.summarizer_backend), "disabled");
   }
   g_metrics.summarizer_threshold = threshold;
   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_record_summarization(size_t input_bytes, size_t output_bytes) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.summarizer_call_count++;
   g_metrics.summarizer_total_in_bytes += input_bytes;
   g_metrics.summarizer_total_out_bytes += output_bytes;
   g_metrics.summarizer_last_in_bytes = input_bytes;
   g_metrics.summarizer_last_out_bytes = output_bytes;
   pthread_mutex_unlock(&g_metrics.mutex);

   float reduction = (input_bytes > 0) ? (1.0f - (float)output_bytes / input_bytes) * 100.0f : 0.0f;
   metrics_log_activity("Summarized: %zu→%zu bytes (%.0f%% reduction)", input_bytes, output_bytes,
                        reduction);
}

/* ============================================================================
 * Audio Status
 * ============================================================================ */

void metrics_update_audio_buffer_fill(float fill_pct) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.audio_buffer_fill_pct = fill_pct;
   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_record_bargein(void) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.bargein_count++;
   pthread_mutex_unlock(&g_metrics.mutex);

   metrics_log_activity("Barge-in detected");
}

void metrics_set_last_asr_text(const char *text, double processing_time_ms) {
   if (!g_initialized || text == NULL) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   strncpy(g_metrics.last_asr_text, text, METRICS_MAX_LOG_LENGTH - 1);
   g_metrics.last_asr_text[METRICS_MAX_LOG_LENGTH - 1] = '\0';
   g_metrics.last_asr_text_time_ms = processing_time_ms;
   /* Sanitize for clean display */
   sanitize_for_display(g_metrics.last_asr_text);
   pthread_mutex_unlock(&g_metrics.mutex);
}

/* ============================================================================
 * ASR Timing
 * ============================================================================ */

void metrics_record_asr_timing(double time_ms, double rtf) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   g_metrics.last_asr_time_ms = time_ms;
   g_metrics.last_asr_rtf = rtf;

   g_metrics.avg_asr_ms = update_average(g_metrics.avg_asr_ms, g_metrics.asr_count, time_ms);
   g_metrics.avg_asr_rtf = update_average(g_metrics.avg_asr_rtf, g_metrics.asr_count, rtf);
   g_metrics.asr_count++;

   pthread_mutex_unlock(&g_metrics.mutex);
}

/* ============================================================================
 * LLM Timing and Tokens
 * ============================================================================ */

void metrics_record_llm_ttft(double ttft_ms) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   g_metrics.last_llm_ttft_ms = ttft_ms;
   g_metrics.avg_llm_ttft_ms = update_average(g_metrics.avg_llm_ttft_ms, g_metrics.llm_count,
                                              ttft_ms);

   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_record_llm_total_time(double total_ms) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   g_metrics.last_llm_total_ms = total_ms;
   g_metrics.avg_llm_total_ms = update_average(g_metrics.avg_llm_total_ms, g_metrics.llm_count,
                                               total_ms);
   g_metrics.llm_count++;

   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_record_llm_tokens(llm_type_t type,
                               cloud_provider_t cloud_provider,
                               int input_tokens,
                               int output_tokens,
                               int cached_tokens) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   if (type == LLM_CLOUD) {
      g_metrics.tokens_cloud_input += input_tokens;
      g_metrics.tokens_cloud_output += output_tokens;
   } else if (type == LLM_LOCAL) {
      g_metrics.tokens_local_input += input_tokens;
      g_metrics.tokens_local_output += output_tokens;
   }
   g_metrics.tokens_cached += cached_tokens;

   // Update current LLM config to reflect last used provider
   // This ensures stats export shows accurate provider info
   g_metrics.current_llm_type = type;
   g_metrics.current_cloud_provider = cloud_provider;

   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_record_llm_query(llm_type_t type) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   g_metrics.queries_total++;
   if (type == LLM_CLOUD) {
      g_metrics.queries_cloud++;
   } else if (type == LLM_LOCAL) {
      g_metrics.queries_local++;
   }

   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_record_fallback(void) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.fallbacks_count++;
   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_record_error(void) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.errors_count++;
   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_update_llm_config(llm_type_t type, cloud_provider_t provider) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   g_metrics.current_llm_type = type;
   g_metrics.current_cloud_provider = provider;
   pthread_mutex_unlock(&g_metrics.mutex);
}

/* ============================================================================
 * TTS Timing
 * ============================================================================ */

void metrics_record_tts_timing(double time_ms) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   g_metrics.last_tts_time_ms = time_ms;
   g_metrics.avg_tts_ms = update_average(g_metrics.avg_tts_ms, g_metrics.tts_count, time_ms);
   g_metrics.tts_count++;

   pthread_mutex_unlock(&g_metrics.mutex);
}

/* ============================================================================
 * Pipeline Timing
 * ============================================================================ */

void metrics_record_pipeline_total(double total_ms) {
   if (!g_initialized) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   g_metrics.last_total_pipeline_ms = total_ms;
   g_metrics.avg_total_pipeline_ms = update_average(g_metrics.avg_total_pipeline_ms,
                                                    g_metrics.pipeline_count, total_ms);
   g_metrics.pipeline_count++;

   pthread_mutex_unlock(&g_metrics.mutex);
}

/* ============================================================================
 * Activity Log
 * ============================================================================ */

void metrics_log_activity(const char *format, ...) {
   if (!g_initialized || format == NULL) {
      return;
   }

   char timestamp[16];
   get_timestamp_str(timestamp, sizeof(timestamp));

   char message[METRICS_MAX_LOG_LENGTH - 20]; /* Leave room for timestamp + separator */
   va_list args;
   va_start(args, format);
   vsnprintf(message, sizeof(message), format, args);
   va_end(args);

   pthread_mutex_lock(&g_metrics.mutex);

   /* Write to circular buffer */
   snprintf(g_metrics.activity_log[g_metrics.log_head], METRICS_MAX_LOG_LENGTH, "%s  %s", timestamp,
            message);

   g_metrics.log_head = (g_metrics.log_head + 1) % METRICS_MAX_LOG_ENTRIES;
   if (g_metrics.log_count < METRICS_MAX_LOG_ENTRIES) {
      g_metrics.log_count++;
   }

   pthread_mutex_unlock(&g_metrics.mutex);
}

void metrics_set_last_user_command(const char *command) {
   if (!g_initialized || command == NULL) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   strncpy(g_metrics.last_user_command, command, METRICS_MAX_LOG_LENGTH - 1);
   g_metrics.last_user_command[METRICS_MAX_LOG_LENGTH - 1] = '\0';
   pthread_mutex_unlock(&g_metrics.mutex);

   /* Also log to activity feed */
   metrics_log_activity("USER: %s", command);
}

void metrics_set_last_ai_response(const char *response) {
   if (!g_initialized || response == NULL) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   strncpy(g_metrics.last_ai_response, response, METRICS_MAX_LOG_LENGTH - 1);
   g_metrics.last_ai_response[METRICS_MAX_LOG_LENGTH - 1] = '\0';
   /* Sanitize for clean display */
   sanitize_for_display(g_metrics.last_ai_response);
   pthread_mutex_unlock(&g_metrics.mutex);

   /* Also log to activity feed (use sanitized version) */
   metrics_log_activity("FRIDAY: %s", g_metrics.last_ai_response);
}

/* ============================================================================
 * Thread-Safe Snapshot
 * ============================================================================ */

void metrics_get_snapshot(dawn_metrics_t *snapshot) {
   if (!g_initialized || snapshot == NULL) {
      return;
   }

   pthread_mutex_lock(&g_metrics.mutex);

   /* Copy entire structure */
   memcpy(snapshot, &g_metrics, sizeof(dawn_metrics_t));

   /* Update state time for current state (live calculation) */
   time_t now = time(NULL);
   if (g_metrics.current_state < METRICS_NUM_STATES) {
      snapshot->state_time[g_metrics.current_state] += (now - g_metrics.state_entry_time);
   }

   pthread_mutex_unlock(&g_metrics.mutex);

   /* Clear the mutex in the snapshot (it's not valid for the copy) */
   memset(&snapshot->mutex, 0, sizeof(pthread_mutex_t));
}

time_t metrics_get_uptime(void) {
   if (!g_initialized) {
      return 0;
   }

   pthread_mutex_lock(&g_metrics.mutex);
   time_t uptime = time(NULL) - g_metrics.session_start_time;
   pthread_mutex_unlock(&g_metrics.mutex);

   return uptime;
}

/* ============================================================================
 * JSON Export
 * ============================================================================ */

int metrics_export_json(const char *filepath) {
   if (!g_initialized || filepath == NULL) {
      return 1;
   }

   dawn_metrics_t snapshot;
   metrics_get_snapshot(&snapshot);

   FILE *fp = fopen(filepath, "w");
   if (fp == NULL) {
      LOG_ERROR("Failed to open metrics export file: %s", filepath);
      return 1;
   }

   time_t now = time(NULL);
   char start_time_str[32];
   char end_time_str[32];
   strftime(start_time_str, sizeof(start_time_str), "%Y-%m-%dT%H:%M:%SZ",
            gmtime(&snapshot.session_start_time));
   strftime(end_time_str, sizeof(end_time_str), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

   fprintf(fp, "{\n");
   fprintf(fp, "  \"session\": {\n");
   fprintf(fp, "    \"start_time\": \"%s\",\n", start_time_str);
   fprintf(fp, "    \"end_time\": \"%s\",\n", end_time_str);
   fprintf(fp, "    \"duration_seconds\": %ld\n", (long)(now - snapshot.session_start_time));
   fprintf(fp, "  },\n");

   fprintf(fp, "  \"queries\": {\n");
   fprintf(fp, "    \"total\": %u,\n", snapshot.queries_total);
   fprintf(fp, "    \"cloud\": %u,\n", snapshot.queries_cloud);
   fprintf(fp, "    \"local\": %u,\n", snapshot.queries_local);
   fprintf(fp, "    \"errors\": %u,\n", snapshot.errors_count);
   fprintf(fp, "    \"fallbacks\": %u\n", snapshot.fallbacks_count);
   fprintf(fp, "  },\n");

   fprintf(fp, "  \"tokens\": {\n");
   fprintf(fp, "    \"cloud\": {\n");
   fprintf(fp, "      \"input\": %lu,\n", (unsigned long)snapshot.tokens_cloud_input);
   fprintf(fp, "      \"output\": %lu,\n", (unsigned long)snapshot.tokens_cloud_output);
   fprintf(fp, "      \"total\": %lu\n",
           (unsigned long)(snapshot.tokens_cloud_input + snapshot.tokens_cloud_output));
   fprintf(fp, "    },\n");
   fprintf(fp, "    \"local\": {\n");
   fprintf(fp, "      \"input\": %lu,\n", (unsigned long)snapshot.tokens_local_input);
   fprintf(fp, "      \"output\": %lu,\n", (unsigned long)snapshot.tokens_local_output);
   fprintf(fp, "      \"total\": %lu\n",
           (unsigned long)(snapshot.tokens_local_input + snapshot.tokens_local_output));
   fprintf(fp, "    },\n");
   fprintf(fp, "    \"cached\": %lu\n", (unsigned long)snapshot.tokens_cached);
   fprintf(fp, "  },\n");

   fprintf(fp, "  \"performance\": {\n");
   fprintf(fp, "    \"averages\": {\n");
   fprintf(fp, "      \"vad_ms\": %.1f,\n", snapshot.avg_vad_ms);
   fprintf(fp, "      \"asr_ms\": %.1f,\n", snapshot.avg_asr_ms);
   fprintf(fp, "      \"asr_rtf\": %.3f,\n", snapshot.avg_asr_rtf);
   fprintf(fp, "      \"llm_ttft_ms\": %.1f,\n", snapshot.avg_llm_ttft_ms);
   fprintf(fp, "      \"llm_total_ms\": %.1f,\n", snapshot.avg_llm_total_ms);
   fprintf(fp, "      \"tts_ms\": %.1f,\n", snapshot.avg_tts_ms);
   fprintf(fp, "      \"total_pipeline_ms\": %.1f\n", snapshot.avg_total_pipeline_ms);
   fprintf(fp, "    },\n");
   fprintf(fp, "    \"last_query\": {\n");
   fprintf(fp, "      \"asr_ms\": %.1f,\n", snapshot.last_asr_time_ms);
   fprintf(fp, "      \"asr_rtf\": %.3f,\n", snapshot.last_asr_rtf);
   fprintf(fp, "      \"llm_ttft_ms\": %.1f,\n", snapshot.last_llm_ttft_ms);
   fprintf(fp, "      \"llm_total_ms\": %.1f,\n", snapshot.last_llm_total_ms);
   fprintf(fp, "      \"tts_ms\": %.1f,\n", snapshot.last_tts_time_ms);
   fprintf(fp, "      \"total_pipeline_ms\": %.1f\n", snapshot.last_total_pipeline_ms);
   fprintf(fp, "    }\n");
   fprintf(fp, "  },\n");

   fprintf(fp, "  \"aec\": {\n");
   fprintf(fp, "    \"enabled\": %s,\n", snapshot.aec_enabled ? "true" : "false");
   fprintf(fp, "    \"calibrated\": %s,\n", snapshot.aec_calibrated ? "true" : "false");
   fprintf(fp, "    \"delay_ms\": %d,\n", snapshot.aec_delay_ms);
   fprintf(fp, "    \"correlation\": %.3f\n", snapshot.aec_correlation);
   fprintf(fp, "  },\n");

   fprintf(fp, "  \"audio\": {\n");
   fprintf(fp, "    \"bargein_count\": %u\n", snapshot.bargein_count);
   fprintf(fp, "  },\n");

   fprintf(fp, "  \"state_distribution_seconds\": {\n");
   fprintf(fp, "    \"SILENCE\": %ld,\n", (long)snapshot.state_time[DAWN_STATE_SILENCE]);
   fprintf(fp, "    \"WAKEWORD_LISTEN\": %ld,\n",
           (long)snapshot.state_time[DAWN_STATE_WAKEWORD_LISTEN]);
   fprintf(fp, "    \"COMMAND_RECORDING\": %ld,\n",
           (long)snapshot.state_time[DAWN_STATE_COMMAND_RECORDING]);
   fprintf(fp, "    \"PROCESS_COMMAND\": %ld\n",
           (long)snapshot.state_time[DAWN_STATE_PROCESS_COMMAND]);
   fprintf(fp, "  },\n");

   const char *llm_type_str = (snapshot.current_llm_type == LLM_LOCAL)
                                  ? "local"
                                  : ((snapshot.current_llm_type == LLM_CLOUD) ? "cloud"
                                                                              : "undefined");
   const char *provider_str = (snapshot.current_cloud_provider == CLOUD_PROVIDER_OPENAI)
                                  ? "openai"
                                  : ((snapshot.current_cloud_provider == CLOUD_PROVIDER_CLAUDE)
                                         ? "claude"
                                         : "none");

   fprintf(fp, "  \"llm_configuration\": {\n");
   fprintf(fp, "    \"type\": \"%s\",\n", llm_type_str);
   fprintf(fp, "    \"provider\": \"%s\"\n", provider_str);
   fprintf(fp, "  }\n");

   fprintf(fp, "}\n");

   fclose(fp);
   LOG_INFO("Metrics exported to %s", filepath);

   return 0;
}
