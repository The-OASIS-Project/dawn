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
 * DAWN Configuration Validation - Config value validation implementation
 */

#include "config/config_validate.h"

#include <stdio.h>
#include <string.h>

/* =============================================================================
 * Helper Macros
 * ============================================================================= */

#define ADD_ERROR(field_name, msg_fmt, ...)                                                     \
   do {                                                                                         \
      if (error_count < (int)max_errors) {                                                      \
         strncpy(errors[error_count].field, field_name, sizeof(errors[error_count].field) - 1); \
         errors[error_count].field[sizeof(errors[error_count].field) - 1] = '\0';               \
         snprintf(errors[error_count].message, sizeof(errors[error_count].message), msg_fmt,    \
                  ##__VA_ARGS__);                                                               \
         error_count++;                                                                         \
      }                                                                                         \
   } while (0)

#define VALIDATE_RANGE_FLOAT(field, value, min, max)                                 \
   do {                                                                              \
      if ((value) < (min) || (value) > (max)) {                                      \
         ADD_ERROR(field, "must be between %.2f and %.2f (got %.4f)", (double)(min), \
                   (double)(max), (double)(value));                                  \
      }                                                                              \
   } while (0)

#define VALIDATE_RANGE_INT(field, value, min, max)                                      \
   do {                                                                                 \
      if ((value) < (min) || (value) > (max)) {                                         \
         ADD_ERROR(field, "must be between %d and %d (got %d)", (int)(min), (int)(max), \
                   (int)(value));                                                       \
      }                                                                                 \
   } while (0)

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

static int string_in_list(const char *str, const char **list, int count) {
   for (int i = 0; i < count; i++) {
      if (strcmp(str, list[i]) == 0)
         return 1;
   }
   return 0;
}

/* =============================================================================
 * Validation Implementation
 * ============================================================================= */

int config_validate(const dawn_config_t *config,
                    const secrets_config_t *secrets,
                    config_error_t *errors,
                    size_t max_errors) {
   if (!config || !errors || max_errors == 0)
      return 0;

   int error_count = 0;

   /* ===== VAD Thresholds (0.0 - 1.0) ===== */
   VALIDATE_RANGE_FLOAT("vad.speech_threshold", config->vad.speech_threshold, 0.0f, 1.0f);
   VALIDATE_RANGE_FLOAT("vad.speech_threshold_tts", config->vad.speech_threshold_tts, 0.0f, 1.0f);
   VALIDATE_RANGE_FLOAT("vad.silence_threshold", config->vad.silence_threshold, 0.0f, 1.0f);

   /* ===== VAD Durations (positive values) ===== */
   if (config->vad.end_of_speech_duration <= 0.0f) {
      ADD_ERROR("vad.end_of_speech_duration", "must be positive (got %.2f)",
                (double)config->vad.end_of_speech_duration);
   }
   if (config->vad.max_recording_duration <= 0.0f) {
      ADD_ERROR("vad.max_recording_duration", "must be positive (got %.2f)",
                (double)config->vad.max_recording_duration);
   }

   /* ===== VAD Chunking ===== */
   if (config->vad.chunking.enabled) {
      if (config->vad.chunking.pause_duration <= 0.0f) {
         ADD_ERROR("vad.chunking.pause_duration", "must be positive when chunking enabled");
      }
      if (config->vad.chunking.min_duration <= 0.0f) {
         ADD_ERROR("vad.chunking.min_chunk_duration", "must be positive when chunking enabled");
      }
      if (config->vad.chunking.max_duration <= config->vad.chunking.min_duration) {
         ADD_ERROR("vad.chunking.max_chunk_duration",
                   "must be greater than min_chunk_duration (%.2f <= %.2f)",
                   (double)config->vad.chunking.max_duration,
                   (double)config->vad.chunking.min_duration);
      }
   }

   /* ===== TTS Length Scale (0.5 - 2.0) ===== */
   VALIDATE_RANGE_FLOAT("tts.length_scale", config->tts.length_scale, 0.5f, 2.0f);

   /* ===== Port Numbers (1 - 65535) ===== */
   VALIDATE_RANGE_INT("mqtt.port", config->mqtt.port, 1, 65535);

   /* ===== Commands Processing Mode (enum) ===== */
   {
      const char *valid_modes[] = { "direct_only", "llm_only", "direct_first" };
      if (!string_in_list(config->commands.processing_mode, valid_modes, 3)) {
         ADD_ERROR("commands.processing_mode",
                   "must be 'direct_only', 'llm_only', or 'direct_first' (got '%s')",
                   config->commands.processing_mode);
      }
   }

   /* ===== LLM Type (enum) ===== */
   {
      const char *valid_types[] = { "cloud", "local" };
      if (!string_in_list(config->llm.type, valid_types, 2)) {
         ADD_ERROR("llm.type", "must be 'cloud' or 'local' (got '%s')", config->llm.type);
      }
   }

   /* ===== LLM Cloud Provider (enum) ===== */
   if (strcmp(config->llm.type, "cloud") == 0) {
      const char *valid_providers[] = { "openai", "claude" };
      if (!string_in_list(config->llm.cloud.provider, valid_providers, 2)) {
         ADD_ERROR("llm.cloud.provider", "must be 'openai' or 'claude' (got '%s')",
                   config->llm.cloud.provider);
      }
      /* Note: API key check removed - handled in dawn.c with graceful fallback to local */
   }

   /* ===== LLM Local Provider (enum) ===== */
   if (config->llm.local.provider[0] != '\0') {
      const char *valid_local_providers[] = { "auto", "ollama", "llama_cpp", "generic" };
      if (!string_in_list(config->llm.local.provider, valid_local_providers, 4)) {
         ADD_ERROR("llm.local.provider",
                   "must be 'auto', 'ollama', 'llama_cpp', or 'generic' (got '%s')",
                   config->llm.local.provider);
      }
   }

   /* ===== LLM Local Endpoint URL Validation ===== */
   if (config->llm.local.endpoint[0] != '\0') {
      /* Must start with http:// or https:// */
      if (strncmp(config->llm.local.endpoint, "http://", 7) != 0 &&
          strncmp(config->llm.local.endpoint, "https://", 8) != 0) {
         ADD_ERROR("llm.local.endpoint", "must start with http:// or https://");
      }
      /* Block cloud metadata endpoints (SSRF prevention) */
      if (strstr(config->llm.local.endpoint, "169.254.169.254") ||
          strstr(config->llm.local.endpoint, "metadata.google.internal") ||
          strstr(config->llm.local.endpoint, "metadata.goog")) {
         ADD_ERROR("llm.local.endpoint", "cloud metadata endpoint not allowed (SSRF protection)");
      }
      /* No injection characters */
      if (strchr(config->llm.local.endpoint, '\n') || strchr(config->llm.local.endpoint, '\r')) {
         ADD_ERROR("llm.local.endpoint", "newline characters not allowed");
      }
   }

   /* ===== Max Tokens (positive) ===== */
   if (config->llm.max_tokens <= 0) {
      ADD_ERROR("llm.max_tokens", "must be positive (got %d)", config->llm.max_tokens);
   }

   /* ===== Network Workers (1-64) ===== */
   VALIDATE_RANGE_INT("network.workers", config->network.workers, 1, 64);
   if (config->network.session_timeout_sec <= 0) {
      ADD_ERROR("network.session_timeout_sec", "must be positive");
   }
   if (config->network.llm_timeout_ms <= 0) {
      ADD_ERROR("network.llm_timeout_ms", "must be positive");
   }

   /* ===== FlareSolverr (if enabled) ===== */
   if (config->url_fetcher.flaresolverr.enabled) {
      VALIDATE_RANGE_INT("url_fetcher.flaresolverr.timeout_sec",
                         config->url_fetcher.flaresolverr.timeout_sec, 1, 300);

      if (config->url_fetcher.flaresolverr.max_response_bytes < 1024) {
         ADD_ERROR("url_fetcher.flaresolverr.max_response_bytes", "must be at least 1KB");
      }
      if (config->url_fetcher.flaresolverr.max_response_bytes > 16 * 1024 * 1024) {
         ADD_ERROR("url_fetcher.flaresolverr.max_response_bytes", "must be at most 16MB");
      }

      if (config->url_fetcher.flaresolverr.endpoint[0] == '\0') {
         ADD_ERROR("url_fetcher.flaresolverr.endpoint", "required when FlareSolverr is enabled");
      }
   }

   /* ===== Barge-in Cooldowns (positive) ===== */
   if (config->audio.bargein.enabled) {
      if (config->audio.bargein.cooldown_ms < 0) {
         ADD_ERROR("audio.bargein.cooldown_ms", "must be non-negative");
      }
      if (config->audio.bargein.startup_cooldown_ms < 0) {
         ADD_ERROR("audio.bargein.startup_cooldown_ms", "must be non-negative");
      }
   }

   /* ===== Summarizer Backend (enum) ===== */
   {
      const char *valid_backends[] = { "disabled", "local", "default", "tfidf" };
      if (!string_in_list(config->search.summarizer.backend, valid_backends, 4)) {
         ADD_ERROR("search.summarizer.backend",
                   "must be 'disabled', 'local', 'default', or 'tfidf' (got '%s')",
                   config->search.summarizer.backend);
      }
   }

   return error_count;
}

void config_print_errors(const config_error_t *errors, int count) {
   if (!errors || count == 0)
      return;

   fprintf(stderr, "Configuration validation errors:\n");
   for (int i = 0; i < count; i++) {
      fprintf(stderr, "  [%s] %s\n", errors[i].field, errors[i].message);
   }
}
