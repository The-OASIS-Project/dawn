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

#include <regex.h>
#include <stdio.h>
#include <string.h>

#include "llm/llm_interface.h"

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

/* NaN-safe range check: the negated-range form catches NaN/+inf/-inf
 * because every comparison with NaN evaluates to false.  The old
 * `value < min || value > max` form let NaN slip through silently.
 * Same shape as include/config/dawn_config.h's CONFIG_CLAMP.
 * Hardened 2026-05-13 (security review LOW-1) — closes ~30 validator
 * sites at once, defense-in-depth against malformed admin-side TOML. */
#define VALIDATE_RANGE_FLOAT(field, value, min, max)                                 \
   do {                                                                              \
      if (!((value) >= (min) && (value) <= (max))) {                                 \
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

   /* ===== ASR cross-device dedup window (0 disables, ceiling defined in dawn_config.h) ===== */
   VALIDATE_RANGE_INT("asr.dedup_window_sec", config->asr.dedup_window_sec, 0,
                      ASR_DEDUP_WINDOW_SEC_MAX);

   /* ===== TTS Length Scale (0.5 - 2.0) ===== */
   VALIDATE_RANGE_FLOAT("tts.length_scale", config->tts.length_scale, 0.5f, 2.0f);

   /* ===== LLM Compaction Thresholds ===== */
   VALIDATE_RANGE_FLOAT("llm.compact_soft_threshold", config->llm.compact_soft_threshold, 0.05f,
                        0.90f);
   VALIDATE_RANGE_FLOAT("llm.compact_hard_threshold", config->llm.compact_hard_threshold, 0.10f,
                        0.95f);
   if (config->llm.compact_soft_threshold >= config->llm.compact_hard_threshold) {
      ADD_ERROR("llm.compact_soft_threshold",
                "must be less than compact_hard_threshold (%.2f >= %.2f)",
                (double)config->llm.compact_soft_threshold,
                (double)config->llm.compact_hard_threshold);
   }
   if (config->llm.compact_provider[0]) {
      const char *valid_providers[] = { "claude", "openai", "gemini", "local" };
      if (!string_in_list(config->llm.compact_provider, valid_providers, 4)) {
         ADD_ERROR("llm.compact_provider",
                   "must be one of: claude, openai, gemini, local (got '%s')",
                   config->llm.compact_provider);
      }
   }

   /* ===== Memory Embedding Weights ===== */
   VALIDATE_RANGE_FLOAT("memory.temporal_weight", config->memory.temporal_weight, 0.0f, 1.0f);
   VALIDATE_RANGE_FLOAT("memory.category_threshold", config->memory.category_threshold, 0.05f,
                        0.95f);
   VALIDATE_RANGE_FLOAT("memory.search_score_floor", config->memory.search_score_floor, 0.0f, 1.0f);
   VALIDATE_RANGE_FLOAT("memory.graph_retrieval.entity_grounding_bonus",
                        config->memory.graph_retrieval.entity_grounding_bonus, 0.0f, 1.0f);
   VALIDATE_RANGE_INT("memory.graph_retrieval.max_facts_per_query",
                      config->memory.graph_retrieval.max_facts_per_query, 1, 200);
   VALIDATE_RANGE_FLOAT("memory.graph_retrieval.entity_bonus",
                        config->memory.graph_retrieval.entity_bonus, 0.0f, 1.0f);
   VALIDATE_RANGE_FLOAT("memory.paraphrase_dedup_threshold",
                        config->memory.paraphrase_dedup_threshold, 0.5f, 1.0f);

   /* ===== Embedding Recomputation ===== */
   VALIDATE_RANGE_INT("memory.embeddings.recompute_batch_size", config->memory.recompute_batch_size,
                      1, 500);
   VALIDATE_RANGE_INT("memory.embeddings.recompute_batch_sleep_ms",
                      config->memory.recompute_batch_sleep_ms, 0, 5000);

   /* ===== Memory Extraction Recovery ===== */
   VALIDATE_RANGE_INT("memory.recovery.idle_threshold_seconds",
                      config->memory.recovery_idle_threshold_seconds, 60, 30 * 86400);
   VALIDATE_RANGE_INT("memory.recovery.max_attempts", config->memory.recovery_max_attempts, 0, 100);
   if (config->memory.recovery_recurring_interval_seconds != 0) {
      VALIDATE_RANGE_INT("memory.recovery.recurring_interval_seconds",
                         config->memory.recovery_recurring_interval_seconds, 60, 30 * 86400);
   }

   /* ===== Per-turn Focus Injection (Phase 1 of dynamic context injection) =====
    * Float weights use the same [0, 5] band as memory.temporal_weight precedent;
    * the band intentionally permits weights >1.0 so a stage can be deliberately
    * over-emphasized during tuning without re-touching this file. */
   VALIDATE_RANGE_INT("memory.focus_injection.focus_budget_bytes",
                      config->memory.focus_injection.focus_budget_bytes, 1024, 65536);
   VALIDATE_RANGE_INT("memory.focus_injection.top_k", config->memory.focus_injection.top_k, 1, 64);
   VALIDATE_RANGE_INT("memory.focus_injection.summary_max_scan",
                      config->memory.focus_injection.summary_max_scan, 256, 16384);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.min_score",
                        config->memory.focus_injection.min_score, 0.0f, 1.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.weight_semantic",
                        config->memory.focus_injection.weight_semantic, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.weight_recency",
                        config->memory.focus_injection.weight_recency, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.weight_importance",
                        config->memory.focus_injection.weight_importance, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.weight_source",
                        config->memory.focus_injection.weight_source, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.source_weights.memory_fact",
                        config->memory.focus_injection.source_weights.memory_fact, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.source_weights.memory_entity",
                        config->memory.focus_injection.source_weights.memory_entity, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.source_weights.memory_relation",
                        config->memory.focus_injection.source_weights.memory_relation, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.source_weights.memory_summary",
                        config->memory.focus_injection.source_weights.memory_summary, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.source_weights.document_chunk",
                        config->memory.focus_injection.source_weights.document_chunk, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.source_weights.calendar_event",
                        config->memory.focus_injection.source_weights.calendar_event, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.source_weights.recent_email",
                        config->memory.focus_injection.source_weights.recent_email, 0.0f, 5.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.source_weights.dawn_background",
                        config->memory.focus_injection.source_weights.dawn_background, 0.0f, 5.0f);
   VALIDATE_RANGE_INT("memory.focus_injection.dedup.recent_window_turns",
                      config->memory.focus_injection.dedup.recent_window_turns, 0, 100);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.dedup.score_uplift_factor",
                        config->memory.focus_injection.dedup.score_uplift_factor, 1.0f, 5.0f);
   /* Lower bound at 0.01 (not 0.0) — the runtime self-guard in
    * focus_apply_dominant_token_penalty silently no-ops at value ≤ 0.0,
    * so accepting 0.0 here would let an operator's slider lie about
    * effect.  Disable the heuristic via the `enabled` flag instead. */
   VALIDATE_RANGE_FLOAT("memory.focus_injection.dominant_token_heuristic.threshold",
                        config->memory.focus_injection.dominant_token_heuristic.threshold, 0.01f,
                        1.0f);
   VALIDATE_RANGE_FLOAT("memory.focus_injection.dominant_token_heuristic.base_penalty",
                        config->memory.focus_injection.dominant_token_heuristic.base_penalty, 0.01f,
                        1.0f);

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

   /* ===== General Mode (enum) ===== */
   if (config->general.mode[0] != '\0') {
      const char *valid_modes[] = { "server" };
      if (!string_in_list(config->general.mode, valid_modes, 1)) {
         ADD_ERROR("general.mode", "must be 'server' or empty (got '%s')", config->general.mode);
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
   if (strcmp(config->llm.type, "cloud") == 0 && config->llm.cloud.provider[0] != '\0') {
      const char *valid_providers[] = { "openai", "claude", "gemini" };
      if (!string_in_list(config->llm.cloud.provider, valid_providers, 3)) {
         ADD_ERROR("llm.cloud.provider", "must be 'openai', 'claude', or 'gemini' (got '%s')",
                   config->llm.cloud.provider);
      }
   }

   /* NOTE: use_openrouter is a separate bool, NOT a provider-string value — it is
    * deliberately not added to valid_providers above.  A gateway-on-but-no-key
    * config is non-fatal (the runtime falls back to local — see llm_init), so it is
    * intentionally not flagged here; config_validate reports hard errors only. */

   /* ===== OpenAI Responses API mode (enum) ===== */
   if (config->llm.cloud.openai_use_responses_api[0] != '\0') {
      const char *valid_modes[] = { "auto", "always", "never" };
      if (!string_in_list(config->llm.cloud.openai_use_responses_api, valid_modes, 3)) {
         ADD_ERROR("llm.cloud.openai_use_responses_api",
                   "must be 'auto', 'always', or 'never' (got '%s')",
                   config->llm.cloud.openai_use_responses_api);
      }
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

   /* ===== Network Workers (1-8, matches WORKER_POOL_MAX_SIZE) ===== */
   VALIDATE_RANGE_INT("network.workers", config->network.workers, 1, 8);
   if (config->network.session_timeout_sec <= 0) {
      ADD_ERROR("network.session_timeout_sec", "must be positive");
   }
   if (config->network.llm_timeout_ms <= 0) {
      ADD_ERROR("network.llm_timeout_ms", "must be positive");
   }
   VALIDATE_RANGE_INT("network.summarization_timeout_ms", config->network.summarization_timeout_ms,
                      1, 600000);
   VALIDATE_RANGE_INT("memory.extraction_timeout_ms", config->memory.extraction_timeout_ms, 1,
                      600000);

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

   /* ===== URL Fetcher Fallback Engine (enum) =====
    * "flaresolverr" (default) | "tavily" | "none". Cross-field rule:
    * fallback = "flaresolverr" but flaresolverr.enabled = false → hard error
    * (the fallback would silently never fire). Missing API key with
    * fallback = "tavily" is NOT a hard error — runtime cascades to
    * FlareSolverr / fails gracefully and surfaces a request-time warning. */
   if (config->url_fetcher.fallback[0] != '\0') {
      const char *valid_fallbacks[] = { "flaresolverr", "tavily", "none" };
      if (!string_in_list(config->url_fetcher.fallback, valid_fallbacks, 3)) {
         ADD_ERROR("url_fetcher.fallback", "must be 'flaresolverr', 'tavily', or 'none' (got '%s')",
                   config->url_fetcher.fallback);
      } else if (strcmp(config->url_fetcher.fallback, "flaresolverr") == 0 &&
                 !config->url_fetcher.flaresolverr.enabled) {
         ADD_ERROR("url_fetcher.fallback",
                   "set to 'flaresolverr' but [url_fetcher.flaresolverr] enabled = false");
      }

      /* Tavily fetch tunables (only validated when selected) */
      if (strcmp(config->url_fetcher.fallback, "tavily") == 0) {
         VALIDATE_RANGE_INT("url_fetcher.tavily.timeout_sec",
                            config->url_fetcher.tavily.timeout_sec, 1, 300);
         if (config->url_fetcher.tavily.max_response_bytes < 1024) {
            ADD_ERROR("url_fetcher.tavily.max_response_bytes", "must be at least 1KB");
         }
         if (config->url_fetcher.tavily.max_response_bytes > 16 * 1024 * 1024) {
            ADD_ERROR("url_fetcher.tavily.max_response_bytes", "must be at most 16MB");
         }
         if (config->url_fetcher.tavily.extract_depth[0] != '\0') {
            const char *valid_depths[] = { "basic", "advanced" };
            if (!string_in_list(config->url_fetcher.tavily.extract_depth, valid_depths, 2)) {
               ADD_ERROR("url_fetcher.tavily.extract_depth",
                         "must be 'basic' or 'advanced' (got '%s')",
                         config->url_fetcher.tavily.extract_depth);
            }
         }
         /* Rate-limit sanity ranges. 0 means "use compile-time default". */
         if (config->url_fetcher.tavily.rate_limit_per_minute != 0) {
            VALIDATE_RANGE_INT("url_fetcher.tavily.rate_limit_per_minute",
                               config->url_fetcher.tavily.rate_limit_per_minute, 1, 10000);
         }
         if (config->url_fetcher.tavily.rate_limit_per_hour != 0) {
            VALIDATE_RANGE_INT("url_fetcher.tavily.rate_limit_per_hour",
                               config->url_fetcher.tavily.rate_limit_per_hour, 1, 100000);
         }
         if (config->url_fetcher.tavily.rate_limit_per_day != 0) {
            VALIDATE_RANGE_INT("url_fetcher.tavily.rate_limit_per_day",
                               config->url_fetcher.tavily.rate_limit_per_day, 1, 1000000);
         }
      }
   }

   /* ===== Search Engine (enum) =====
    * "searxng" (default) | "tavily" | "disabled". Empty allowed (legacy
    * behavior — treated as searxng). Missing API key with engine=tavily is
    * NOT a hard error; the dispatcher in web_search.c gracefully falls back
    * to SearXNG. */
   if (config->search.engine[0] != '\0') {
      const char *valid_engines[] = { "searxng", "tavily", "disabled" };
      if (!string_in_list(config->search.engine, valid_engines, 3)) {
         ADD_ERROR("search.engine", "must be 'searxng', 'tavily', or 'disabled' (got '%s')",
                   config->search.engine);
      }
   }

   /* secrets pointer kept in signature for future cross-field checks
    * (e.g. soft-warning when engine = "tavily" but no API key set). No
    * current consumer reads it directly here. */
   (void)secrets;

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

   /* ===== Silent-Observe Provider (single allowlist owned by
    * llm_silent_observe.c — see llm_silent_observe_provider_is_valid). ===== */
   if (config->llm.silent_observe.provider[0] != '\0' &&
       !llm_silent_observe_provider_is_valid(config->llm.silent_observe.provider)) {
      ADD_ERROR("llm.silent_observe.provider",
                "must be 'local', 'ollama', 'openai', 'claude', 'anthropic', or 'gemini' "
                "(got '%s')",
                config->llm.silent_observe.provider);
   }

   /* ===== OTA ===== */
   if (config->ota.enabled) {
      VALIDATE_RANGE_INT("ota.download_token_ttl_sec", config->ota.download_token_ttl_sec, 5, 3600);
      if (config->ota.release_dir[0] == '\0') {
         ADD_ERROR("ota.release_dir", "must be set when ota.enabled = true");
      }
   }

   /* ===== MCP bridge servers ===== */
   for (int i = 0; i < config->mcp.server_count && i < MCP_SERVERS_MAX; i++) {
      const mcp_server_config_t *srv = &config->mcp.servers[i];
      if (strcmp(srv->transport, "http+sse") != 0) {
         ADD_ERROR("mcp.server.transport", "server '%s' transport must be 'http+sse' (got '%s')",
                   srv->alias, srv->transport);
      }
      if (strcmp(srv->capabilities, "dangerous") != 0 &&
          strcmp(srv->capabilities, "read_only") != 0) {
         ADD_ERROR("mcp.server.capabilities",
                   "server '%s' capabilities must be 'dangerous' or 'read_only' (got '%s')",
                   srv->alias, srv->capabilities);
      }
      /* 0 = use the built-in default (30s request, no idle close). */
      VALIDATE_RANGE_INT("mcp.server.request_timeout_seconds", srv->request_timeout_seconds, 0,
                         3600);
      VALIDATE_RANGE_INT("mcp.server.idle_close_seconds", srv->idle_close_seconds, 0, 86400);
      /* tls_verify=false is only permitted when [mcp] dev_mode = true (sec-M5). */
      if (!srv->tls_verify && !config->mcp.dev_mode) {
         ADD_ERROR("mcp.server.tls_verify",
                   "server '%s' has tls_verify=false but [mcp] dev_mode is not true", srv->alias);
      }
      if (srv->enabled) {
         if (srv->alias[0] == '\0') {
            ADD_ERROR("mcp.server.alias", "enabled MCP server at index %d has no alias", i);
         }
         if (srv->url[0] == '\0') {
            ADD_ERROR("mcp.server.url", "enabled MCP server '%s' has no url", srv->alias);
         }
      }
   }

   /* ===== Code projects (coding harness) — only when enabled ===== */
   if (config->code_projects.enabled) {
      VALIDATE_RANGE_INT("code_projects.max_repo_size_mb", config->code_projects.max_repo_size_mb,
                         1, 1024 * 1024);
      VALIDATE_RANGE_INT("code_projects.max_file_count", config->code_projects.max_file_count, 1,
                         100000000);
      VALIDATE_RANGE_INT("code_projects.max_path_depth", config->code_projects.max_path_depth, 1,
                         255);
      VALIDATE_RANGE_INT("code_projects.clone_depth", config->code_projects.clone_depth, 0, 1);
      if (config->code_projects.source_root[0] == '\0') {
         ADD_ERROR("code_projects.source_root", "must be set when code_projects is enabled");
      }
      if (config->code_projects.import_user_required[0] != '\0' &&
          strcmp(config->code_projects.import_user_required, "admin") != 0) {
         ADD_ERROR("code_projects.import_user_required", "must be empty or 'admin' (got '%s')",
                   config->code_projects.import_user_required);
      }
      if (config->code_projects.default_index_mode[0] != '\0' &&
          strcmp(config->code_projects.default_index_mode, "full") != 0) {
         ADD_ERROR("code_projects.default_index_mode", "must be 'full' (got '%s')",
                   config->code_projects.default_index_mode);
      }
      if (config->code_projects.allowed_host_pattern[0] != '\0') {
         regex_t re;
         int rerr = regcomp(&re, config->code_projects.allowed_host_pattern,
                            REG_EXTENDED | REG_NOSUB);
         if (rerr != 0) {
            ADD_ERROR("code_projects.allowed_host_pattern", "is not a valid POSIX extended regex");
         } else {
            regfree(&re);
         }
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
