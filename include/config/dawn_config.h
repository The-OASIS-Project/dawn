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
#define CONFIG_API_KEY_MAX 256
#define CONFIG_CREDENTIAL_MAX 64

/* Maximum number of URL fetcher whitelist entries */
#define URL_FETCHER_MAX_WHITELIST 16
#define URL_FETCHER_ENTRY_MAX 128 /* Max length of each whitelist entry */

/* Named audio devices configuration */
#define AUDIO_NAMED_DEVICE_MAX 8  /* Max named audio devices */
#define AUDIO_DEVICE_ALIAS_MAX 10 /* Max aliases per device */
#define AUDIO_ALIAS_LEN 64        /* Max length of each alias */

/* =============================================================================
 * General Configuration
 * ============================================================================= */
typedef struct {
   char ai_name[CONFIG_NAME_MAX];  /* Wake word (lowercase) */
   char log_file[CONFIG_PATH_MAX]; /* Empty = stdout, or path */
   char room[64];                  /* Room name for local voice context (e.g. "office") */
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

/**
 * @brief Type of named audio device (capture/playback)
 */
typedef enum {
   AUDIO_DEV_TYPE_CAPTURE,
   AUDIO_DEV_TYPE_PLAYBACK
} audio_device_type_t;

/**
 * @brief Named audio device for voice command switching
 *
 * Allows users to switch between audio devices using voice commands
 * like "switch to microphone" or "use headphones".
 */
typedef struct {
   char name[CONFIG_NAME_MAX];                            /* User-facing name */
   audio_device_type_t type;                              /* capture or playback */
   char device[CONFIG_DEVICE_MAX];                        /* Backend device ID */
   char aliases[AUDIO_DEVICE_ALIAS_MAX][AUDIO_ALIAS_LEN]; /* Alternative names */
   int alias_count;                                       /* Number of aliases */
} audio_named_device_t;

typedef struct {
   char backend[16];                        /* "auto", "pulseaudio", "alsa" */
   char capture_device[CONFIG_DEVICE_MAX];  /* Device name */
   char playback_device[CONFIG_DEVICE_MAX]; /* Device name */
   unsigned int output_rate;                /* Playback sample rate: 44100 or 48000 */
   unsigned int output_channels;            /* Playback channels: 2 (stereo for dmix) */
   bargein_config_t bargein;

   /* Named device mappings for voice commands */
   audio_named_device_t named_devices[AUDIO_NAMED_DEVICE_MAX];
   int named_device_count;
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
   char models_path[CONFIG_PATH_MAX]; /* Path to TTS model files */
   char voice_model[128];             /* Piper voice model name */
   float length_scale;                /* Speaking rate: <1.0 = faster, >1.0 = slower */
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
/* Maximum models per provider in the configurable model list */
#define LLM_CLOUD_MAX_MODELS 8
#define LLM_CLOUD_MODEL_NAME_MAX 64

/* Default fallback models when no models are configured
 * Updated: 2026-01 - Update these when new model generations are released */
#define LLM_DEFAULT_OPENAI_MODEL "gpt-5-mini"
#define LLM_DEFAULT_CLAUDE_MODEL "claude-sonnet-4-5"
#define LLM_DEFAULT_GEMINI_MODEL "gemini-2.5-flash"

typedef struct {
   char provider[16];              /* "openai", "claude", or "gemini" */
   char endpoint[CONFIG_PATH_MAX]; /* Empty = default, or custom endpoint */
   bool vision_enabled;            /* Model supports vision/image analysis */

   /* Configurable model lists for quick controls dropdown */
   char openai_models[LLM_CLOUD_MAX_MODELS][LLM_CLOUD_MODEL_NAME_MAX];
   int openai_models_count;
   int openai_default_model_idx; /* Index into openai_models for default */

   char claude_models[LLM_CLOUD_MAX_MODELS][LLM_CLOUD_MODEL_NAME_MAX];
   int claude_models_count;
   int claude_default_model_idx; /* Index into claude_models for default */

   char gemini_models[LLM_CLOUD_MAX_MODELS][LLM_CLOUD_MODEL_NAME_MAX];
   int gemini_models_count;
   int gemini_default_model_idx; /* Index into gemini_models for default */
} llm_cloud_config_t;

typedef struct {
   char endpoint[CONFIG_PATH_MAX]; /* Local llama-server endpoint */
   char model[CONFIG_NAME_MAX];    /* Optional model name */
   bool vision_enabled;            /* Model supports vision (e.g., LLaVA, Qwen-VL) */
   char provider[16];              /* "auto", "ollama", "llama_cpp", "generic" */
} llm_local_config_t;

#define LLM_TOOLS_MAX_CONFIGURED 32
#define LLM_TOOL_NAME_MAX 64

typedef struct {
   char mode[16]; /* "native", "command_tags", or "disabled" (default: native) */

   /* Per-tool enable lists (empty + configured = none enabled) */
   char local_enabled[LLM_TOOLS_MAX_CONFIGURED][LLM_TOOL_NAME_MAX];
   int local_enabled_count;
   bool local_enabled_configured; /* true if explicitly set in config (even if empty) */
   char remote_enabled[LLM_TOOLS_MAX_CONFIGURED][LLM_TOOL_NAME_MAX];
   int remote_enabled_count;
   bool remote_enabled_configured; /* true if explicitly set in config (even if empty) */
} llm_tools_config_t;

/* Default token budget levels for reasoning_effort dropdown */
#define LLM_THINKING_BUDGET_LOW_DEFAULT 1024
#define LLM_THINKING_BUDGET_MEDIUM_DEFAULT 8192
#define LLM_THINKING_BUDGET_HIGH_DEFAULT 16384

typedef struct {
   char mode[16];            /* "disabled", "enabled", "auto" */
   char reasoning_effort[8]; /* "low", "medium", "high" for reasoning models
                              * Controls token budget via dropdown.
                              * OpenAI o-series/GPT-5: also maps to reasoning_effort param
                              * Gemini 2.5+/3.x: maps to reasoning_effort; NOTE: Gemini cannot
                              *   fully disable reasoning - "disabled" mode uses "low" effort */
   int budget_low;           /* Token budget for "low" effort (default: 1024) */
   int budget_medium;        /* Token budget for "medium" effort (default: 8192) */
   int budget_high;          /* Token budget for "high" effort (default: 16384) */
} llm_thinking_config_t;

typedef struct {
   char type[16];  /* "cloud" or "local" */
   int max_tokens; /* Max response tokens */
   llm_cloud_config_t cloud;
   llm_local_config_t local;
   llm_tools_config_t tools;       /* Native tool/function calling settings */
   llm_thinking_config_t thinking; /* Extended thinking/reasoning settings */
   float summarize_threshold;      /* Compact conversation at this % of context (default: 0.80) */
   bool conversation_logging;      /* Save chat history to log files (default: false) */
} llm_config_t;

/* =============================================================================
 * Search Configuration
 * ============================================================================= */
typedef struct {
   char backend[16];       /* "disabled", "local", "default", "tfidf" */
   size_t threshold_bytes; /* Summarize results larger than this */
   size_t target_words;    /* Target summary length (for LLM backends) */
   float target_ratio;     /* Target sentence ratio for TF-IDF (0.0-1.0, e.g., 0.2 = 20%) */
} summarizer_file_config_t;

/* Maximum configurable title filters */
#define SEARCH_MAX_TITLE_FILTERS 16
#define SEARCH_TITLE_FILTER_MAX 64

typedef struct {
   char engine[32];                /* Search engine name */
   char endpoint[CONFIG_PATH_MAX]; /* SearXNG instance URL */
   summarizer_file_config_t summarizer;

   /* Title filters - exclude results with these terms (case-insensitive) */
   char title_filters[SEARCH_MAX_TITLE_FILTERS][SEARCH_TITLE_FILTER_MAX];
   int title_filters_count;
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
 * Network Configuration (shared settings for sessions, workers, LLM timeouts)
 * ============================================================================= */
typedef struct {
   int workers;             /* Concurrent processing threads */
   int session_timeout_sec; /* Idle session expiry */
   int llm_timeout_ms;      /* Per-request LLM timeout */
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
   bool enabled;                        /* Enable WebUI server */
   int port;                            /* HTTP/WebSocket port (default: 3000) */
   int max_clients;                     /* Max concurrent WebSocket clients */
   int audio_chunk_ms;                  /* Audio chunk size in ms (100-500, default: 200) */
   int workers;                         /* ASR worker threads for voice input (default: 1) */
   char www_path[CONFIG_PATH_MAX];      /* Path to static files */
   char bind_address[CONFIG_NAME_MAX];  /* Bind address (default: 127.0.0.1) */
   bool https;                          /* Enable HTTPS (required for mic on LAN) */
   char ssl_cert_path[CONFIG_PATH_MAX]; /* Path to SSL certificate (.pem) */
   char ssl_key_path[CONFIG_PATH_MAX];  /* Path to SSL private key (.pem) */
} webui_config_t;

/* =============================================================================
 * Images Configuration
 * ============================================================================= */
typedef struct {
   int retention_days; /* Auto-delete images after N days (0 = never, default: 0) */
   int max_size_mb;    /* Max image size in MB (default: 4) */
   int max_per_user;   /* Max images per user (default: 1000) */
} images_config_t;

/* =============================================================================
 * Shutdown Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;        /* Enable voice/command shutdown (default: false) */
   char passphrase[64]; /* Required passphrase, empty = no passphrase required */
} shutdown_config_t;

/* =============================================================================
 * Memory Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;                 /* Enable memory system */
   int context_budget_tokens;    /* Max tokens for memory context (~800) */
   char extraction_provider[16]; /* LLM provider for extraction */
   char extraction_model[64];    /* Model for extraction */

   /* Pruning settings */
   bool pruning_enabled;             /* Enable automatic fact pruning */
   int prune_superseded_days;        /* Delete superseded facts older than N days */
   int prune_stale_days;             /* Delete stale facts not accessed in N days */
   float prune_stale_min_confidence; /* Only prune stale facts below this confidence */

   /* Voice conversation idle timeout */
   int conversation_idle_timeout_min; /* Minutes before auto-save (default: 15, 0=disabled) */
   int default_voice_user_id;         /* User ID for local/DAP conversations (default: 1) */
} memory_config_t;

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
   char data_dir[CONFIG_PATH_MAX]; /* Data directory for databases (default: ~/.local/share/dawn) */
   char music_dir[CONFIG_PATH_MAX]; /* Music library location */
} paths_config_t;

/* =============================================================================
 * Music Configuration
 * ============================================================================= */
typedef struct {
   int scan_interval_minutes; /* Minutes between rescans (0 = disabled, default: 60) */

   /* Streaming settings (music.streaming section) */
   bool streaming_enabled;         /* Enable WebUI music streaming (default: true) */
   char streaming_quality[16];     /* Default quality: voice/standard/high/hifi */
   char streaming_bitrate_mode[8]; /* vbr or cbr */
} music_config_t;

/* =============================================================================
 * Secrets Configuration (loaded separately from secrets.toml)
 * ============================================================================= */
typedef struct {
   char openai_api_key[CONFIG_API_KEY_MAX];
   char claude_api_key[CONFIG_API_KEY_MAX];
   char gemini_api_key[CONFIG_API_KEY_MAX];
   char mqtt_username[CONFIG_CREDENTIAL_MAX];
   char mqtt_password[CONFIG_CREDENTIAL_MAX];

   /* SmartThings authentication (two modes supported):
    * 1. Personal Access Token (PAT): Set access_token only - simpler, no refresh
    * 2. OAuth2: Set client_id + client_secret - tokens stored in
    *    ~/.config/dawn/smartthings_tokens.json and auto-refresh */
   char smartthings_access_token[CONFIG_API_KEY_MAX]; /* PAT mode (preferred) */
   char smartthings_client_id[CONFIG_CREDENTIAL_MAX]; /* OAuth2 mode */
   char smartthings_client_secret[CONFIG_API_KEY_MAX];
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
   images_config_t images;
   memory_config_t memory;
   shutdown_config_t shutdown;
   debug_config_t debug;
   paths_config_t paths;
   music_config_t music;
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
