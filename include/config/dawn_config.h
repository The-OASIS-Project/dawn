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
 * DAWN Configuration System - Main configuration struct definitions
 *
 * Thread Safety: Configuration is loaded once at startup and read-only during
 * runtime. No mutex required for concurrent reads after initialization.
 */

#ifndef DAWN_CONFIG_H
#define DAWN_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Buffer Size Constants
 * ============================================================================= */
#define CONFIG_PATH_MAX 256
#define CONFIG_NAME_MAX 64
#define CONFIG_DEVICE_MAX 128
#define CONFIG_DESCRIPTION_MAX 2048
#define CONFIG_API_KEY_MAX 128
#define CONFIG_CREDENTIAL_MAX 64

/* Maximum number of URL fetcher whitelist entries */
#define URL_FETCHER_MAX_WHITELIST 16
#define URL_FETCHER_ENTRY_MAX 128 /* Max length of each whitelist entry */

/* =============================================================================
 * General Configuration
 * ============================================================================= */
typedef struct {
   char ai_name[CONFIG_NAME_MAX];  /* Wake word (lowercase) */
   char log_file[CONFIG_PATH_MAX]; /* Empty = stdout, or path */
} general_config_t;

/* =============================================================================
 * Persona Configuration
 * ============================================================================= */
typedef struct {
   char description[CONFIG_DESCRIPTION_MAX]; /* System prompt (can be large) */
} persona_config_t;

/* =============================================================================
 * Localization Configuration
 * ============================================================================= */
typedef struct {
   char location[128];             /* Default location for weather/context */
   char timezone[CONFIG_NAME_MAX]; /* Empty = system default */
   char units[16];                 /* "imperial" or "metric" */
} localization_config_t;

/* =============================================================================
 * Audio Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;            /* Allow interrupting TTS with speech */
   int cooldown_ms;         /* Keep high VAD threshold after TTS stops */
   int startup_cooldown_ms; /* Block barge-in when TTS starts */
} bargein_config_t;

typedef struct {
   char backend[16];                        /* "auto", "pulseaudio", "alsa" */
   char capture_device[CONFIG_DEVICE_MAX];  /* Device name */
   char playback_device[CONFIG_DEVICE_MAX]; /* Device name */
   unsigned int output_rate;                /* Playback sample rate: 44100 or 48000 */
   unsigned int output_channels;            /* Playback channels: 2 (stereo for dmix) */
   bargein_config_t bargein;
} audio_config_t;

/* =============================================================================
 * VAD (Voice Activity Detection) Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;         /* Enable natural pause detection */
   float pause_duration; /* Silence duration for chunk boundary */
   float min_duration;   /* Minimum speech before creating chunk */
   float max_duration;   /* Force chunk boundary after this duration */
} vad_chunking_config_t;

typedef struct {
   float speech_threshold;       /* Probability to detect speech start (0.0-1.0) */
   float speech_threshold_tts;   /* Higher threshold during TTS */
   float silence_threshold;      /* Probability for end-of-utterance */
   float end_of_speech_duration; /* Seconds of silence to end recording */
   float max_recording_duration; /* Maximum recording length (seconds) */
   int preroll_ms;               /* Audio buffer before VAD trigger */
   vad_chunking_config_t chunking;
} vad_config_t;

/* =============================================================================
 * ASR (Automatic Speech Recognition) Configuration
 * ============================================================================= */
typedef struct {
   char model[CONFIG_NAME_MAX];       /* Whisper: "tiny", "base", "small", "medium" */
   char models_path[CONFIG_PATH_MAX]; /* Path to model files */
} asr_config_t;

/* =============================================================================
 * TTS (Text-to-Speech) Configuration
 * ============================================================================= */
typedef struct {
   char voice_model[128]; /* Piper voice model name */
   float length_scale;    /* Speaking rate: <1.0 = faster, >1.0 = slower */
} tts_config_t;

/* =============================================================================
 * Commands Configuration
 * ============================================================================= */
typedef struct {
   char processing_mode[16]; /* "direct_only", "llm_only", "direct_first" */
} commands_config_t;

/* =============================================================================
 * LLM (Large Language Model) Configuration
 * ============================================================================= */
typedef struct {
   char provider[16];              /* "openai" or "claude" */
   char model[CONFIG_NAME_MAX];    /* Model name */
   char endpoint[CONFIG_PATH_MAX]; /* Empty = default, or custom endpoint */
   bool vision_enabled;            /* Model supports vision/image analysis */
} llm_cloud_config_t;

typedef struct {
   char endpoint[CONFIG_PATH_MAX]; /* Local llama-server endpoint */
   char model[CONFIG_NAME_MAX];    /* Optional model name */
   bool vision_enabled;            /* Model supports vision (e.g., LLaVA, Qwen-VL) */
} llm_local_config_t;

typedef struct {
   char type[16];  /* "cloud" or "local" */
   int max_tokens; /* Max response tokens */
   llm_cloud_config_t cloud;
   llm_local_config_t local;
} llm_config_t;

/* =============================================================================
 * Search Configuration
 * ============================================================================= */
typedef struct {
   char backend[16];       /* "disabled", "local", "default" */
   size_t threshold_bytes; /* Summarize results larger than this */
   size_t target_words;    /* Target summary length */
} summarizer_file_config_t;

typedef struct {
   char engine[32];                /* Search engine name */
   char endpoint[CONFIG_PATH_MAX]; /* SearXNG instance URL */
   summarizer_file_config_t summarizer;
} search_config_t;

/* =============================================================================
 * URL Fetcher Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;                   /* Auto-fallback on 403 errors */
   char endpoint[CONFIG_PATH_MAX]; /* FlareSolverr API endpoint */
   int timeout_sec;                /* Request timeout */
   size_t max_response_bytes;      /* Max response size */
} flaresolverr_config_t;

typedef struct {
   char whitelist[URL_FETCHER_MAX_WHITELIST][URL_FETCHER_ENTRY_MAX]; /* Static whitelist */
   int whitelist_count; /* Number of whitelist entries */
   flaresolverr_config_t flaresolverr;
} url_fetcher_config_t;

/* =============================================================================
 * MQTT Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;
   char broker[CONFIG_PATH_MAX];
   int port;
} mqtt_config_t;

/* =============================================================================
 * Network Audio Server Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;               /* Enable network audio server */
   char host[CONFIG_NAME_MAX]; /* Bind address */
   int port;                   /* Listen port */
   int workers;                /* Concurrent processing threads */
   int socket_timeout_sec;     /* Client timeout */
   int session_timeout_sec;    /* Idle session expiry */
   int llm_timeout_ms;         /* Per-request LLM timeout */
} network_config_t;

/* =============================================================================
 * TUI (Terminal UI) Configuration
 * ============================================================================= */
typedef struct {
   bool enabled; /* Enable TUI dashboard */
} tui_config_t;

/* =============================================================================
 * WebUI Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;                       /* Enable WebUI server */
   int port;                           /* HTTP/WebSocket port (default: 8080) */
   int max_clients;                    /* Max concurrent WebSocket clients */
   char www_path[CONFIG_PATH_MAX];     /* Path to static files */
   char bind_address[CONFIG_NAME_MAX]; /* Bind address (default: 127.0.0.1) */
} webui_config_t;

/* =============================================================================
 * Debug Configuration
 * ============================================================================= */
typedef struct {
   bool mic_record;                   /* Record raw microphone input */
   bool asr_record;                   /* Record ASR input audio */
   bool aec_record;                   /* Record AEC processed audio */
   char record_path[CONFIG_PATH_MAX]; /* Directory for debug recordings */
} debug_config_t;

/* =============================================================================
 * Paths Configuration
 * ============================================================================= */
typedef struct {
   char music_dir[CONFIG_PATH_MAX];       /* Music library location */
   char commands_config[CONFIG_PATH_MAX]; /* Device/command mappings */
} paths_config_t;

/* =============================================================================
 * Secrets Configuration (loaded separately from secrets.toml)
 * ============================================================================= */
typedef struct {
   char openai_api_key[CONFIG_API_KEY_MAX];
   char claude_api_key[CONFIG_API_KEY_MAX];
   char mqtt_username[CONFIG_CREDENTIAL_MAX];
   char mqtt_password[CONFIG_CREDENTIAL_MAX];
} secrets_config_t;

/* =============================================================================
 * Main Configuration Struct
 * ============================================================================= */
typedef struct {
   general_config_t general;
   persona_config_t persona;
   localization_config_t localization;
   audio_config_t audio;
   vad_config_t vad;
   asr_config_t asr;
   tts_config_t tts;
   commands_config_t commands;
   llm_config_t llm;
   search_config_t search;
   url_fetcher_config_t url_fetcher;
   mqtt_config_t mqtt;
   network_config_t network;
   tui_config_t tui;
   webui_config_t webui;
   debug_config_t debug;
   paths_config_t paths;
} dawn_config_t;

/* =============================================================================
 * Global Configuration Instances (read-only after initialization)
 * ============================================================================= */
extern dawn_config_t g_config;
extern secrets_config_t g_secrets;

/* =============================================================================
 * Configuration API
 * ============================================================================= */

/**
 * @brief Initialize config with default values
 *
 * Sets all fields to their compile-time defaults. Call this before parsing
 * any config files to ensure all values have sensible defaults.
 *
 * @param config Config struct to initialize
 */
void config_set_defaults(dawn_config_t *config);

/**
 * @brief Initialize secrets with empty/default values
 *
 * @param secrets Secrets struct to initialize
 */
void config_set_secrets_defaults(secrets_config_t *secrets);

/**
 * @brief Get the global config instance (read-only after init)
 *
 * @return Pointer to the global config
 */
const dawn_config_t *config_get(void);

/**
 * @brief Get the global secrets instance (read-only after init)
 *
 * @return Pointer to the global secrets
 */
const secrets_config_t *config_get_secrets(void);

/**
 * @brief Clean up any config resources
 *
 * Call at program shutdown. Currently a no-op since all config
 * uses static allocation, but reserved for future use.
 */
void config_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_CONFIG_H */
