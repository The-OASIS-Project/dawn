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
 * DAWN Configuration System - Default value initialization
 *
 * All default values match the compile-time constants in dawn.h and dawn.c
 */

#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"

/* =============================================================================
 * Global Configuration Instances
 * ============================================================================= */
dawn_config_t g_config;
secrets_config_t g_secrets;

/* =============================================================================
 * Helper Macros
 * ============================================================================= */
#define SAFE_COPY(dst, src)                   \
   do {                                       \
      strncpy((dst), (src), sizeof(dst) - 1); \
      (dst)[sizeof(dst) - 1] = '\0';          \
   } while (0)

/* =============================================================================
 * Default Values - matching dawn.h and dawn.c
 * ============================================================================= */
void config_set_defaults(dawn_config_t *config) {
   if (!config)
      return;

   memset(config, 0, sizeof(*config));

   /* General */
   SAFE_COPY(config->general.ai_name, "friday");
   config->general.log_file[0] = '\0'; /* Empty = stdout */

   /* Persona - empty means use compile-time default from dawn.h */
   config->persona.description[0] = '\0';

   /* Localization */
   config->localization.location[0] = '\0'; /* No default location */
   config->localization.timezone[0] = '\0'; /* System default */
   SAFE_COPY(config->localization.units, "imperial");

   /* Audio */
   SAFE_COPY(config->audio.backend, "auto");
   SAFE_COPY(config->audio.capture_device, "default");
   SAFE_COPY(config->audio.playback_device, "default");
   config->audio.output_rate = 44100; /* CD quality, native for most music */
   config->audio.output_channels = 2; /* Stereo required for dmix compatibility */

   /* Audio barge-in */
   config->audio.bargein.enabled = true;
   config->audio.bargein.cooldown_ms = 1500;        /* VAD_TTS_COOLDOWN_MS */
   config->audio.bargein.startup_cooldown_ms = 300; /* VAD_TTS_STARTUP_COOLDOWN_MS */

   /* VAD - matching defines in dawn.c */
   config->vad.speech_threshold = 0.5f;        /* VAD_SPEECH_THRESHOLD */
   config->vad.speech_threshold_tts = 0.92f;   /* VAD_SPEECH_THRESHOLD_TTS */
   config->vad.silence_threshold = 0.3f;       /* VAD_SILENCE_THRESHOLD */
   config->vad.end_of_speech_duration = 1.2f;  /* VAD_END_OF_SPEECH_DURATION */
   config->vad.max_recording_duration = 30.0f; /* VAD_MAX_RECORDING_DURATION */
   config->vad.preroll_ms = 500;

   /* VAD Chunking - matching defines in dawn.c */
   config->vad.chunking.enabled = true;
   config->vad.chunking.pause_duration = 0.3f; /* VAD_CHUNK_PAUSE_DURATION */
   config->vad.chunking.min_duration = 1.0f;   /* VAD_MIN_CHUNK_DURATION */
   config->vad.chunking.max_duration = 10.0f;  /* VAD_MAX_CHUNK_DURATION */

   /* ASR */
   SAFE_COPY(config->asr.model, "base");
   SAFE_COPY(config->asr.models_path, "models/whisper.cpp");

   /* TTS */
   SAFE_COPY(config->tts.models_path, "models");
   SAFE_COPY(config->tts.voice_model, "en_GB-alba-medium");
   config->tts.length_scale = 0.85f;

   /* Commands */
   SAFE_COPY(config->commands.processing_mode, "direct_first");

   /* LLM */
   SAFE_COPY(config->llm.type, "cloud");
   config->llm.max_tokens = 4096; /* GPT_MAX_TOKENS from dawn.h */

   /* LLM Cloud */
   SAFE_COPY(config->llm.cloud.provider, "openai");
   SAFE_COPY(config->llm.cloud.openai_model, "gpt-4o");
   SAFE_COPY(config->llm.cloud.claude_model, "claude-sonnet-4-20250514");
   config->llm.cloud.endpoint[0] = '\0'; /* Empty = use default */
   config->llm.cloud.vision_enabled = true;

   /* LLM Local */
   SAFE_COPY(config->llm.local.endpoint, "http://127.0.0.1:8080");
   config->llm.local.model[0] = '\0';        /* Server decides */
   config->llm.local.vision_enabled = false; /* Most local models don't support vision */

   /* Search */
   SAFE_COPY(config->search.engine, "searxng");
   SAFE_COPY(config->search.endpoint, "http://127.0.0.1:8384");

   /* Search Summarizer */
   SAFE_COPY(config->search.summarizer.backend, "disabled");
   config->search.summarizer.threshold_bytes = 3072;
   config->search.summarizer.target_words = 600;

   /* URL Fetcher - whitelist is zeroed by memset */
   config->url_fetcher.whitelist_count = 0;

   /* FlareSolverr */
   config->url_fetcher.flaresolverr.enabled = false;
   SAFE_COPY(config->url_fetcher.flaresolverr.endpoint, "http://127.0.0.1:8191/v1");
   config->url_fetcher.flaresolverr.timeout_sec = 60;
   config->url_fetcher.flaresolverr.max_response_bytes = 4 * 1024 * 1024; /* 4MB */

   /* MQTT - matching dawn.h */
   config->mqtt.enabled = true;
   SAFE_COPY(config->mqtt.broker, "127.0.0.1"); /* MQTT_IP */
   config->mqtt.port = 1883;                    /* MQTT_PORT */

   /* Network */
   config->network.enabled = false;
   SAFE_COPY(config->network.host, "0.0.0.0");
   config->network.port = 5000;
   config->network.workers = 4;
   config->network.socket_timeout_sec = 30;
   config->network.session_timeout_sec = 1800;  // 30 minutes
   config->network.llm_timeout_ms = 30000;

   /* TUI */
   config->tui.enabled = false;

   /* WebUI */
   config->webui.enabled = false;
   config->webui.port = 3000; /* "I love you 3000" */
   config->webui.max_clients = 4;
   config->webui.audio_chunk_ms = 200; /* 200ms chunks for streaming audio */
   config->webui.workers = 1;          /* ASR workers for voice input (1 = minimal) */
   SAFE_COPY(config->webui.www_path, "www");
   SAFE_COPY(config->webui.bind_address, "0.0.0.0");
   config->webui.https = false;
   config->webui.ssl_cert_path[0] = '\0';
   config->webui.ssl_key_path[0] = '\0';

   /* Debug */
   config->debug.mic_record = false;
   config->debug.asr_record = false;
   config->debug.aec_record = false;
   SAFE_COPY(config->debug.record_path, "/tmp");

   /* Paths - matching dawn.h */
   SAFE_COPY(config->paths.music_dir, "~/Music");
   SAFE_COPY(config->paths.commands_config, "commands_config_nuevo.json");
}

void config_set_secrets_defaults(secrets_config_t *secrets) {
   if (!secrets)
      return;

   memset(secrets, 0, sizeof(*secrets));
   /* All secrets default to empty strings */
}

const dawn_config_t *config_get(void) {
   return &g_config;
}

const secrets_config_t *config_get_secrets(void) {
   return &g_secrets;
}

void config_cleanup(void) {
   /* Currently a no-op - all config uses static allocation */
   /* Reserved for future use if dynamic resources are added */
   (void)0;
}
