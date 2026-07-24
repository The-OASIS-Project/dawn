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
 * Thread Safety: Configuration is loaded at startup and may be mutated at runtime
 * via WebUI settings behind s_config_rwlock (in webui_server.c). Readers should
 * snapshot values into local variables when consistency across multiple fields matters.
 *
 * ADDING A FIELD OR SECTION HERE IS STEP 1 OF ~9 — see docs/CONFIGURATION_GUIDE.md.
 * Declaring the struct member, defaulting it, and parsing it is NOT enough: a
 * setting that config_write_toml() (config_env.c) never emits is silently DELETED
 * from the user's dawn.toml the first time they save any WebUI setting, because a
 * settings save rewrites the whole file from this struct. The build stays clean and
 * the tests stay green when you forget — it has shipped three times. config_to_json()
 * (GET), config_write_toml() (persist), and webui_config.c's POST handler always move
 * together; tests/test_config_roundtrip.c guards it.
 */

#ifndef DAWN_CONFIG_H
#define DAWN_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Utility Macros
 * ============================================================================= */
/* CONFIG_CLAMP: forces a numeric config value into [lo, hi].  The
 * negated-range guard `!(val >= lo && val <= hi)` catches NaN, +inf,
 * and -inf — IEEE 754 NaN compares false to everything so a naïve
 * `if (val < lo) ... if (val > hi) ...` would leave NaN unchanged and
 * silently propagate into runtime checks (where `score >= NaN` is also
 * false, disabling features without an error log).  Out-of-range
 * non-NaN values fall back to whichever bound they violated; NaN/inf
 * fall back to `lo` (deterministic safe default). */
#ifndef CONFIG_CLAMP
#define CONFIG_CLAMP(val, lo, hi)                               \
   do {                                                         \
      if (!((val) >= (lo) && (val) <= (hi))) {                  \
         (val) = ((val) > (hi)) ? (hi) : (lo); /* NaN → lo */ \
      }                                                         \
   } while (0)
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

/* Default for memory.paraphrase_dedup_threshold — the extraction-time cosine
 * cutoff above which a new fact is merged into an existing one rather than
 * stored.  Single source of truth: config_defaults.c seeds the field with it,
 * and the interactive 'remember' write-time advisory uses it as the upper bound
 * of its near-duplicate band (and as the fallback when the config is invalid). */
#define MEMORY_PARAPHRASE_DEDUP_DEFAULT 0.92f

/* =============================================================================
 * General Configuration
 * ============================================================================= */
typedef struct {
   char ai_name[CONFIG_NAME_MAX];  /* Wake word (lowercase) */
   char log_file[CONFIG_PATH_MAX]; /* Empty = stdout, or path */
   char room[64];                  /* Room name for local voice context (e.g. "office") */
   char mode[16];                  /* "" = full (default), "server" = no local audio */
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

/* Cross-device utterance dedup window (seconds).  Keep in sync with the
 * `dedup_window_sec` documentation in dawn.toml.example and the min/max in
 * www/js/ui/settings/schema.js (JS can't include this header). */
#define ASR_DEDUP_WINDOW_SEC_DEFAULT 4
#define ASR_DEDUP_WINDOW_SEC_MAX 60

typedef struct {
   char model[CONFIG_NAME_MAX];       /* Whisper: "tiny", "base", "small", "medium" */
   char models_path[CONFIG_PATH_MAX]; /* Path to model files */
   int dedup_window_sec;              /* Cross-device utterance dedup window (s); 0 disables */
   /* Prompt hint injected on voice-input surfaces warning the LLM that its
    * input was speech-transcribed and may contain homophone/mis-recognition
    * errors to interpret from context.  Empty = built-in default
    * (DEFAULT_ASR_DISAMBIGUATION_HINT in dawn.h). */
   char disambiguation_hint[CONFIG_DESCRIPTION_MAX];
} asr_config_t;

/* =============================================================================
 * TTS (Text-to-Speech) Configuration
 * ============================================================================= */
typedef struct {
   char models_path[CONFIG_PATH_MAX]; /* Path to TTS model files */
   char voice_model[128];             /* Piper voice model name */
   float length_scale;                /* Speaking rate: <1.0 = faster, >1.0 = slower */
   /* Spoken-output shaping directives injected on voice surfaces (reply is
    * read aloud by TTS, not shown on a screen).  Empty = built-in default
    * (DEFAULT_VOICE_OUTPUT_DIRECTIVE / _WEBUI in dawn.h). */
   char voice_directive[CONFIG_DESCRIPTION_MAX];       /* Satellites + local mic (firm) */
   char voice_directive_webui[CONFIG_DESCRIPTION_MAX]; /* WebUI voice turns (screen leeway) */
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
 * Updated: 2026-02 - Update these when new model generations are released */
#define LLM_DEFAULT_OPENAI_MODEL "gpt-5.4"
#define LLM_DEFAULT_CLAUDE_MODEL "claude-sonnet-4-6"
#define LLM_DEFAULT_GEMINI_MODEL "gemini-2.5-flash"
#define LLM_DEFAULT_OPENROUTER_MODEL "anthropic/claude-sonnet-4.6"

typedef struct {
   char provider[16];              /* "openai", "claude", or "gemini" */
   char endpoint[CONFIG_PATH_MAX]; /* Empty = default, or custom endpoint */
   bool vision_enabled;            /* Model supports vision/image analysis */

   /* OpenRouter gateway mode: when true, ALL cloud traffic (main chat AND the
    * auxiliary extraction/compaction/silent-observe/scheduler calls) routes
    * through OpenRouter using openrouter_api_key, regardless of the provider
    * field above.  Direct-provider settings are hidden in the WebUI but
    * preserved.  See docs/arch/subsystems/llm.md "OpenRouter gateway". */
   bool use_openrouter;

   /* OpenAI endpoint selection: "auto" (route gpt-5.4* to /v1/responses),
    * "always" (force /v1/responses for all OpenAI cloud calls),
    * "never" (legacy chat-completions only — gpt-5.4 will be broken). */
   char openai_use_responses_api[16];

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

   /* OpenRouter model list (used when use_openrouter is true).  These are the
    * curated "favorites" shown in the header model switcher; the full live
    * catalog (Phase 2) is browsed in Settings.  IDs are "vendor/model"
    * (e.g. "anthropic/claude-sonnet-4"). */
   char openrouter_models[LLM_CLOUD_MAX_MODELS][LLM_CLOUD_MODEL_NAME_MAX];
   int openrouter_models_count;
   int openrouter_default_model_idx; /* Index into openrouter_models for default */
} llm_cloud_config_t;

typedef struct {
   char endpoint[CONFIG_PATH_MAX]; /* Local llama-server endpoint */
   char model[CONFIG_NAME_MAX];    /* Optional model name */
   bool vision_enabled;            /* Model supports vision (e.g., LLaVA, Qwen-VL) */
   char provider[16];              /* "auto", "ollama", "llama_cpp", "generic" */
} llm_local_config_t;

#define LLM_TOOLS_MAX_CONFIGURED 32
#define LLM_TOOL_NAME_MAX 64

typedef struct llm_tools_config {
   char mode[16]; /* "native", "command_tags", or "disabled" (default: native) */

   /* Per-tool enable lists — WHITELIST (legacy): only listed (non-dangerous) tools
    * are enabled. Also the opt-in mechanism for TOOL_CAP_DANGEROUS tools under
    * either model. Empty + configured = none enabled. */
   char local_enabled[LLM_TOOLS_MAX_CONFIGURED][LLM_TOOL_NAME_MAX];
   int local_enabled_count;
   bool local_enabled_configured; /* true if explicitly set in config (even if empty) */
   char remote_enabled[LLM_TOOLS_MAX_CONFIGURED][LLM_TOOL_NAME_MAX];
   int remote_enabled_count;
   bool remote_enabled_configured; /* true if explicitly set in config (even if empty) */

   /* Per-tool disable lists — BLOCKLIST (default-on): when set, all non-dangerous
    * tools are enabled EXCEPT those listed (so a newly-added tool is on by
    * default). Takes precedence over the enable list for non-dangerous tools.
    * Dangerous tools still require explicit enable-list opt-in regardless. */
   char local_disabled[LLM_TOOLS_MAX_CONFIGURED][LLM_TOOL_NAME_MAX];
   int local_disabled_count;
   bool local_disabled_configured;
   char remote_disabled[LLM_TOOLS_MAX_CONFIGURED][LLM_TOOL_NAME_MAX];
   int remote_disabled_count;
   bool remote_disabled_configured;
} llm_tools_config_t;

/* Default token budget levels for reasoning_effort dropdown */
#define LLM_THINKING_BUDGET_LOW_DEFAULT 1024
#define LLM_THINKING_BUDGET_MEDIUM_DEFAULT 8192
#define LLM_THINKING_BUDGET_HIGH_DEFAULT 16384
#define LLM_THINKING_BUDGET_XHIGH_DEFAULT 32768

typedef struct {
   char mode[16];            /* "disabled", "enabled" (legacy "auto" also accepted) */
   char reasoning_effort[8]; /* "none"/"low"/"medium"/"high"/"xhigh" for reasoning models
                              * OpenAI gpt-5.4+ and gpt-5.2: full range incl. none/xhigh.
                              * OpenAI gpt-5/5.1 + Gemini OpenAI-compat: clamps xhigh→high
                              *   and (for non-5.1+) none→low at request-build time.
                              * Claude: maps to budget_low/medium/high/xhigh via budget switch
                              *   (see llm_get_effective_budget_tokens). */
   int budget_low;           /* Token budget for "low"   effort (default: 1024)  */
   int budget_medium;        /* Token budget for "medium" effort (default: 8192)  */
   int budget_high;          /* Token budget for "high"  effort (default: 16384) */
   int budget_xhigh;         /* Token budget for "xhigh" effort (default: 32768) */
} llm_thinking_config_t;

/* Silent-observe primitive (Phase 0 of Dynamic Context Injection).
 * Provider config is dedicated so background observations can run on a cheap
 * model without affecting user-facing chat.  See src/llm/llm_silent_observe.c. */
typedef struct {
   char provider[16]; /* "local" | "ollama" | "openai" | "claude" | "anthropic" | "gemini" */
   char model[64];    /* Model name (provider-specific, direct-API naming) */
   char openrouter_model[64]; /* Model when OpenRouter gateway is on ("vendor/model");
                               * empty = use main OpenRouter default */
} llm_silent_observe_config_t;

typedef struct {
   char type[16];  /* "cloud" or "local" */
   int max_tokens; /* Max response tokens */
   llm_cloud_config_t cloud;
   llm_local_config_t local;
   llm_tools_config_t tools;                   /* Native tool/function calling settings */
   llm_thinking_config_t thinking;             /* Extended thinking/reasoning settings */
   llm_silent_observe_config_t silent_observe; /* Silent-observe call mode */
   float summarize_threshold; /* PARSER ONLY — legacy TOML key, derives compact_*. Do not read at
                                 runtime. */
   float compact_soft_threshold; /* Async compaction trigger (default: 0.60) */
   float compact_hard_threshold; /* Blocking compaction trigger (default: 0.85) */
   bool compact_use_session;     /* Use session's provider for compaction (default: true) */
   char compact_provider[32];    /* Dedicated compaction provider: openai/claude/gemini/local */
   char compact_model[128];      /* Dedicated compaction model name (direct-API naming) */
   char compact_openrouter_model[128]; /* Compaction model when OpenRouter gateway is on
                                        * ("vendor/model"); empty = use main OpenRouter default */
   bool conversation_logging;          /* Save chat history to log files (default: false) */
   bool rate_limit_enabled;            /* Throttle cloud API calls (default: true) */
   int rate_limit_rpm;                 /* Max cloud API calls per minute (default: 40) */
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

/* Tavily /extract config (HTTPS endpoint hard-coded inside adapter).
 * Used when url_fetcher.fallback = "tavily". API key in secrets.toml. */
typedef struct {
   int timeout_sec;           /* Request timeout (default 30) */
   size_t max_response_bytes; /* Max raw_content size (default 1 MB) */
   /* "basic" (1 credit, faithful page text) or "advanced" (2 credits, smart
    * article-body extraction with nav/sidebar/footer stripped). Default
    * "advanced" — heavily-chromed sites (TipRanks, Yahoo, etc.) dominate
    * the first 8 KB with menu links under basic, leaving article body
    * behind the hard truncate. */
   char extract_depth[16];
   /* Rate-limit caps. Per-user (minute + hour) bounds runaway tool loops;
    * global (day) bounds total monthly-quota burn across the deployment.
    * Defaults: 10/min, 100/hr per user, 500/day global — sized for Tavily's
    * 1000/mo free tier. 0 means "use compile-time default". Both /search
    * and /extract count against the same buckets (Tavily quota is unified). */
   int rate_limit_per_minute;
   int rate_limit_per_hour;
   int rate_limit_per_day;
} tavily_fetch_config_t;

typedef struct {
   char whitelist[URL_FETCHER_MAX_WHITELIST][URL_FETCHER_ENTRY_MAX]; /* Static whitelist */
   int whitelist_count; /* Number of whitelist entries */
   /* Fallback engine selection when direct libcurl fetch fails.
    * "flaresolverr" (default) | "tavily" | "none". When "tavily" is selected
    * and the API key is missing, runtime falls back to flaresolverr-or-none. */
   char fallback[16];
   flaresolverr_config_t flaresolverr;
   tavily_fetch_config_t tavily;
} url_fetcher_config_t;

/* =============================================================================
 * MQTT Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;
   char broker[CONFIG_PATH_MAX];
   int port;
   bool tls;
   char tls_ca_cert[CONFIG_PATH_MAX];
   char tls_cert_path[CONFIG_PATH_MAX];
   char tls_key_path[CONFIG_PATH_MAX];
} mqtt_config_t;

/* =============================================================================
 * Network Configuration (shared settings for sessions, workers, LLM timeouts)
 * ============================================================================= */
typedef struct {
   int workers;                  /* Concurrent processing threads */
   int session_timeout_sec;      /* Idle session expiry */
   int llm_timeout_ms;           /* Per-request LLM timeout */
   int summarization_timeout_ms; /* LLM timeout for context summarization (default 180s) */
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
   char www_path[CONFIG_PATH_MAX];      /* Path to static files */
   char bind_address[CONFIG_NAME_MAX];  /* Bind address (default: 127.0.0.1) */
   bool https;                          /* Enable HTTPS (required for mic on LAN) */
   char ssl_cert_path[CONFIG_PATH_MAX]; /* Path to SSL certificate (.pem) */
   char ssl_key_path[CONFIG_PATH_MAX];  /* Path to SSL private key (.pem) */
   int export_max_messages;             /* Max messages per export (0 = unlimited, default: 5000) */
   char
       export_format[CONFIG_NAME_MAX]; /* Default export format: "json" or "html" (default: json) */
} webui_config_t;

/* =============================================================================
 * Images Configuration (storage retention settings)
 * ============================================================================= */
typedef struct {
   int retention_days; /* Auto-delete DEFAULT images after N days (0 = never, default: 0) */
   int max_size_mb;    /* Max image size in MB (default: 4) */
   int max_per_user;   /* Max images per user (default: 1000) */
   int cache_size_mb;  /* LRU cache cap for CACHE-retention images in MB (default: 200) */
} images_config_t;

/* =============================================================================
 * Documents Configuration (upload and extraction limits)
 * ============================================================================= */
typedef struct {
   int max_file_size_kb;      /* Max upload size in KB (default: 512, range: 64-10240) */
   int max_documents;         /* Max concurrent attached docs (default: 5, range: 1-20) */
   int max_pages;             /* Max PDF pages to extract (default: 100, range: 1-500) */
   int max_extracted_size_kb; /* Max extracted text in KB (default: 1024, range: 128-4096) */
   int max_index_size_kb;     /* Max document text for RAG indexing in KB (default: 2048) */
   int max_indexed_documents; /* Max indexed documents per user (default: 50, range: 1-500) */
   /* v61 hybrid document search (lexical BM25 + semantic cosine + phrase bonus).
    * fts_*_weight are FTS5 per-column BM25 weights (label weighted high so an
    * exact-label match dominates); hybrid_*_weight fuse the lexical and semantic
    * channels; phrase_bonus_weight scales the ordered/contiguous-phrase bonus;
    * search_min_score is the post-fusion relevance gate (relocated from the old
    * DOC_SEARCH_MIN_SCORE constant — recalibrated for the re-weighted score). */
   float fts_label_weight;      /* BM25 weight for the label/filename column (default: 3.0) */
   float fts_body_weight;       /* BM25 weight for the chunk-text column (default: 1.0) */
   float hybrid_keyword_weight; /* Lexical (BM25) fusion weight (default: 0.3, range: 0-1) */
   float hybrid_vector_weight;  /* Semantic (cosine) fusion weight (default: 0.7, range: 0-1) */
   float phrase_bonus_weight;   /* Ordered/contiguous phrase-bonus scale (default: 0.25, 0-1) */
   float search_min_score;      /* Post-fusion relevance gate (default: 0.3, range: 0-1) */
   /* v62 versioning: a snapshot of a note/document's content is archived before
    * every destructive mutation (overwrite, edit/append, delete).  Retention is
    * the older of two bounds — age in days, and a per-document keep-last-N cap. */
   int version_retention_days; /* Drop archived versions older than this (default: 14, 0 = off) */
   int version_keep_per_doc;   /* Per-document cap on retained versions (default: 10) */
   /* v68 original-file storage: keep the raw uploaded PDF/DOCX in the blob store
    * (re-extraction, OCR, media gallery).  Keep-forever by default; the orphan
    * sweep reclaims only never-indexed uploads + post-delete orphans. */
   bool originals_enabled;      /* Store original uploads (default: true) */
   int original_retention_days; /* Age-delete originals after N days (default: 0 = forever) */
   int max_original_size_mb;    /* Per-original byte cap in MB (default: 10) */
   int max_originals_per_user;  /* Per-user original count cap (default: 200) */
   int max_originals_total_mb_per_user; /* Per-user aggregate cap in MB (default: 2048) */
   int original_grace_minutes;          /* Orphan grace before sweep reclaims (default: 45) */
} documents_config_t;

/* =============================================================================
 * Vision Configuration (per-upload image size and dimension limits)
 * ============================================================================= */
typedef struct {
   int max_image_size_kb;     /* Max image upload size in KB (default: 4096, range: 512-16384) */
   int max_dimension;         /* Max image dimension in px (default: 1024, range: 256-4096) */
   int max_images;            /* Max images per message (default: 5, range: 1-10) */
   int capture_history_count; /* Tool-captured images (e.g. the `viewing` camera tool) retained
                               * in conversation history for follow-up questions. 0 = unlimited
                               * (bounded only by context compaction, not per-capture eviction),
                               * default: 1, range: 0-50. */
} vision_config_t;

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

/* Per-source weights consumed by the focus-injection ranker.  Fixed
 * shape (no dynamic keys) for v1 — Phase 2 / 1d may add fields.  Future:
 * a parallel-array shape may be needed if 1d introduces sub-weights
 * (e.g., calendar "this hour" vs "this week", per-email-folder).  Don't
 * redesign now — fixed shape is correct for 1b. */
typedef struct {
   float memory_fact;
   float memory_entity;
   float memory_relation;
   float memory_summary;
   float document_chunk;
   float calendar_event;
   float recent_email;
   float dawn_background;
} focus_source_weights_t;

/* Dedup bookkeeping for per-turn focus.  Read by the parser into the
 * struct in Phase 1b — actual per-session enforcement lives in 1f. */
typedef struct {
   int recent_window_turns;   /* Re-inject only if not seen in last N turns */
   float score_uplift_factor; /* Re-inject if current score > previous * factor */
} focus_dedup_config_t;

typedef struct {
   bool enabled;            /* Master enable; default false until 1c/1d ship */
   int focus_budget_bytes;  /* Byte cap on the assembled focus block (per turn) */
   int top_k;               /* Maximum candidates retained after ranking */
   float min_score;         /* Floor — anything below is dropped */
   bool classifier_enabled; /* RAGRoute-style source classifier;
                               default false (off, opt-in after probe) */
   float weight_semantic;   /* Cross-source ranker weights — Generative
                               Agents / CrewAI three-factor formula */
   float weight_recency;
   float weight_importance;
   float weight_source;  /* Multiplier on per-source weights below */
   int summary_max_scan; /* Rows the semantic summary adapter inspects
                            per turn before the cosine ranking heap.
                            Bump above 4096 only if a user's corpus
                            grows past that and the cliff causes drops. */
   focus_source_weights_t source_weights;
   focus_dedup_config_t dedup;
   /* Dominant-token over-inclusion heuristic (Phase 1j re-bench + Phase B
    * reranker workstream).  Mitigates the failure mode where a query has
    * a low-IDF dominant token (e.g. "favorite restaurant", "doctor blood
    * test"), > threshold of the candidate pool shares that token, and
    * the right-answer summary uses specific vocabulary.  Bench-validated
    * at canonical params via benchmarks/focus_probe_cases.json cases
    * 16-18 (severe_dominant_token_*) — see docs/RERANKER_PHASE_A_DESIGN.md. */
   struct {
      bool enabled;
      float threshold;    /* Pool-fraction share to call a token "dominant" */
      float base_penalty; /* Max composite-score penalty per candidate */
   } dominant_token_heuristic;
} focus_injection_config_t;

/* Unified cross-source `recall` tool (docs/CROSS_TOOL_RECALL_DESIGN.md).
 * Deep on-demand gather across all focus sources at a budget LARGER than the
 * per-turn injection, kept in a SEPARATE block so tuning the deep path never
 * disturbs the tuned per-turn `focus_injection` values.  recall_tool copies
 * top_k/min_score/budget_bytes into a focus_limits_t via DESIGNATED initializers
 * (mapping is by-name, not positional — field order here need not match
 * focus_limits_t); per_source_max rides focus_compose_ex's existing param. */
typedef struct {
   int top_k;          /* Max candidates retained after ranking (deep gather) */
   int budget_bytes;   /* Byte cap on the assembled recall result text         */
   float min_score;    /* Floor — lower than per-turn so weaker hits surface    */
   int per_source_max; /* Per-adapter fan-out cap before ranking.
                        * INVARIANT: per_source_max * MAX_FOCUS_SOURCES <= 256
                        * or the dominant-token heuristic self-disables. */
} recall_config_t;

typedef struct {
   bool enabled;                         /* Enable memory system */
   int context_budget_tokens;            /* Max tokens for memory context (~800) */
   int source_budget_chars;              /* Max chars of verbatim source excerpts per
                                          * memory_callback "search"/"recent" call when
                                          * with_source=true.  Default 3072 (~768 tokens).
                                          * Bench validation (May 2026, LoCoMo Haiku):
                                          * 0=baseline 0.368, 1024=+3.5pp, 3072=+7.7pp,
                                          * 6144=+9.5pp, 12288=+TBD recall_generation.
                                          * Tune up for higher answer quality, down for
                                          * lower per-call token cost.  Hard-clamped at
                                          * MEMORY_SOURCE_BUDGET_MAX (32 KiB). */
   char extraction_provider[16];         /* LLM provider for extraction */
   char extraction_model[64];            /* Model for extraction (direct-API naming) */
   char extraction_openrouter_model[64]; /* Extraction model when OpenRouter gateway is on
                                          * ("vendor/model"); empty = use main OpenRouter default */
   int extraction_timeout_ms;            /* LLM timeout for fact extraction (default 120s) */

   /* Pruning settings */
   bool pruning_enabled;             /* Enable automatic fact pruning */
   int prune_superseded_days;        /* Delete superseded facts older than N days */
   int prune_stale_days;             /* Delete stale facts not accessed in N days */
   float prune_stale_min_confidence; /* Only prune stale facts below this confidence */

   /* Fact expiry / ephemerality settings (v58, C3).  Off by default until the
    * recall check validates it on a live DB.  See atlas/dawn/memory/EPHEMERALITY_DESIGN.md. */
   bool expire_enabled;    /* Gate: extraction sets expires_at, retrieval guard + nightly
                            * prune_expired honor it.  Off → expires_at never written and the
                            * retrieval guard admits everything (instant, non-mutating). */
   int expire_grace_days;  /* Days past a fact's reference date before it expires (soft-hides) */
   int prune_expired_days; /* Days an expired fact stays (recoverable) before hard delete;
                            * 0 = hard-expire on the reference date (no buffer) */

   /* Voice conversation idle timeout */
   int conversation_idle_timeout_min; /* Minutes before auto-save (default: 15, 0=disabled) */
   int default_voice_user_id;         /* User ID for local/DAP conversations (default: 1) */

   /* Decay settings (Phase 5) */
   bool decay_enabled;               /* Enable nightly confidence decay */
   int decay_hour;                   /* Hour to run (0-23, default: 2) */
   float decay_inferred_weekly;      /* Weekly multiplier for inferred facts (0.95) */
   float decay_explicit_weekly;      /* Weekly multiplier for explicit facts (0.98) */
   float decay_preference_weekly;    /* Weekly multiplier for preferences (0.97) */
   float decay_inferred_floor;       /* Min confidence for inferred facts (0.0) */
   float decay_explicit_floor;       /* Min confidence for explicit facts (0.50) */
   float decay_preference_floor;     /* Min confidence for preferences (0.40) */
   float decay_prune_threshold;      /* Delete facts below this (0.25) */
   int summary_retention_days;       /* Delete summaries older than N days (30) */
   float access_reinforcement_boost; /* Confidence boost on access (0.05) */

   /* Embeddings (semantic search) */
   char embedding_provider[16];        /* "onnx", "ollama", "openai", "" (disabled) */
   char embedding_model[64];           /* HTTP providers: model name */
   char embedding_endpoint[256];       /* HTTP providers: base URL */
   float embedding_keyword_weight;     /* hybrid search keyword weight (0.0-1.0) */
   float embedding_vector_weight;      /* hybrid search vector weight (0.0-1.0) */
   bool embedding_backfill_on_startup; /* backfill existing facts on startup */
   /* Embedding model identity — bump this string whenever MODEL_PATH changes so
    * the recompute worker detects the swap and re-indexes stored embeddings. */
   char model_id[64];
   bool recompute_on_model_change; /* re-embed all stored vectors on model swap */
   int recompute_batch_size;       /* rows per batch (default 50) */
   int recompute_batch_sleep_ms;   /* sleep between batches in ms (default 100) */
   /* Temporal-query scoring.  When the query contains a parsed temporal
    * expression (e.g., "in 2020", "last week"), each candidate fact's score
    * gets `temporal_weight * gaussian_proximity(fact.created_at)` added.
    * 0 disables; 0.10–0.30 is the useful range. */
   float temporal_weight;
   /* Hard temporal filter on memory.search results.  When the query contains
    * a parsed temporal expression AND this flag is on, retrieved facts are
    * stable-partitioned by whether their source conversation's `anchor_date`
    * falls inside the parsed window — in-window facts move to the front,
    * out-of-window facts move to the back but are NOT dropped.  Distinct
    * from `temporal_weight` (soft Gaussian decay on fact.created_at):
    * filter targets conv.anchor_date (when the user *spoke* about the
    * event) instead of fact.created_at (extraction time).  Empirical basis:
    * Mem0 v2 +29.6pp on temporal questions via metadata filter (May 2026
    * research synthesis).  Off-by-default for A/B; flip in
    * `config_defaults.c` after bench validation. */
   bool temporal_filter_enabled;
   /* Reciprocal Rank Fusion retrieval.  When on, hybrid search builds three
    * independent rank lists (semantic cosine, keyword multi-token, temporal
    * proximity when applicable) over the same candidate pool and scores
    * each fact by RRF: score = Σ 1/(60 + rank_i).  Replaces the weighted-
    * sum composite (kw_weight * kw + vec_weight * cosine + temporal_weight
    * * proximity).  Empirical basis: Mem0 v2 + Hindsight TEMPR both cite
    * RRF over parallel channels as their primary retrieval lever.
    * Off-by-default for A/B against the existing composite. */
   bool rrf_enabled;
   /* Minimal-format memory_text output.  When on, memory.search emits a
    * Mem0/ENGRAM-style compact layout:
    *   - "Memories:" / "Preferences:" / "Conversation summaries:" headers
    *     replace "FACTS (N):" / "PREFERENCES:" / "CONVERSATION SUMMARIES (K):"
    *   - Per-fact [ID:M], confidence, time-ago annotations stripped
    *   - Per-fact source excerpts demoted to a single "Source evidence:"
    *     block at the end (top-3 excerpts, one per fact)
    *   - ENTITIES/RELATIONS graph block omitted from the generator surface
    *
    * Empirical basis: ENGRAM @ 916 tokens beats Mem0 @ 7K on LongMemEval
    * SOTA (2026 research synthesis).  Goal: cut memory_text from ~3K tokens
    * to ~2K, reducing the LLM's "lost in the middle" surface area.
    *
    * Off-by-default for A/B.  Earlier v4 attempt regressed -1pp on 28-Q
    * sample with a smaller cut (14.8% size reduction); v5 is the more
    * aggressive shape the research synthesis recommended. */
   bool memory_text_minimal;
   /* BM25 keyword retrieval via SQLite FTS5 + Porter2 stemming.  When on,
    * memory_fact_search_hybrid routes the keyword channel through
    * memory_db_fact_search_bm25 (one FTS5 MATCH per query, BM25 ranking,
    * sigmoid-normalized to [0, 1]) instead of the legacy tokenize +
    * multi-LIKE + match-count path.  Phase 1 of the Mem0 Architectural
    * Parity program (docs/MEM0_ARCHITECTURAL_PARITY.md).  Algorithm and
    * sigmoid params adapted from Mem0 (Apache-2.0); see NOTICE.
    * Off-by-default for A/B; flip after bench validation. */
   bool bm25_enabled;
   /* Category backfill — cosine similarity above which a fact is assigned to a
    * non-general category.  Calibrated for MiniLM-L6 at 0.25; other models
    * (mpnet, nomic, OpenAI) may need different values. */
   float category_threshold;
   /* Graph-retrieval (Phase 2.b of cat-3 work) — parallel candidate
    * source for memory.search alongside hybrid keyword + cosine search.
    * Extracts proper-noun entities from the query, resolves to canonical
    * memory_entities, fans out one hop via fact-linked memory_relations,
    * and surfaces the resulting fact_ids as additional candidates with
    * a flat entity-grounding score.  Targets cat-3 entity-specific
    * lookup queries like "What country did Jolene buy snake Seraphim?"
    * where cosine over-broadens on the noun and the gold answer sits
    * at rank 50+.  See memory_graph_retrieval.h for the pipeline. */
   struct {
      bool enabled; /* default true; disable to A/B against pure hybrid */
      /* Per-fact score assigned to graph-anchored candidates.  Default 0.4
       * sits above the 0.30 search_score_floor (graph candidates survive
       * the floor by construction) and below typical strong hybrid hits
       * (~0.5-0.7 cosine + keyword), so the merge keeps graph candidates
       * present without dominating the ranking.  Bench-tune. */
      float entity_grounding_bonus;
      /* Total fan-out cap across all seed entities per query.  Top-degree
       * LoCoMo entities have 50-80 incoming + outgoing relations; 30 is
       * comfortable headroom for the typical 1-3 seeds while bounded
       * against pathological multi-entity queries. */
      int max_facts_per_query;
      /* Phase 2 Step 1 (May 14, 2026): query-aware scoring of graph
       * candidates.  When true (default), graph candidates are re-scored
       * against the query using cosine(query, fact) + entity_bonus.
       * Replaces the flat entity_grounding_bonus that was disconnected
       * from query content.  Set false to revert to flat-bonus scoring
       * for config-toggle ablation.
       *
       * See docs/PHASE_2_GRAPH_RETRIEVAL_DESIGN.md §2C Step 1. */
      bool use_query_scoring;
      /* Small additive bonus applied to query-scored graph candidates
       * (when use_query_scoring=true).  Distinguishes graph-rescued
       * candidates from pure-hybrid candidates at otherwise-tied scores.
       * Default 0.10 — small enough that hybrid relevance dominates,
       * large enough that an entity-grounded near-tie wins.  Range
       * [0.0, 1.0]; 0.0 reproduces pure-hybrid scoring on the graph
       * candidate pool (no graph preference). */
      float entity_bonus;
   } graph_retrieval;

   /* Memory-tool search score floor.  Drops facts whose hybrid score
    * (kw_weight * kw_score + vec_weight * cosine + temporal boost) falls
    * below this value before they reach the LLM.  Closes the "I don't
    * remember" gate against marginal-cosine fabrication: at 0 the LLM
    * sees every cosine hit above the embeddings layer's 0.01 near-zero
    * cut; at 0.30 (default) hybrid-vector-only marginal hits below the
    * natural kw_weight=0.30 boundary are dropped silently.  Only applies
    * to the non-category branch of memory_action_search() and the per-
    * turn focus-injection fact adapter — category-filtered searches are
    * SQL-LIKE grounded and need no floor.  0.30 = `kw_weight * 1.0` is
    * the cleanest cutoff (preserves every legitimate single-token
    * keyword match); drop to 0.10-0.20 if your corpus shows over-
    * filtering, or push higher if weak fabrications persist.
    *
    * SINGLE-KNOB DESIGN: this value gates BOTH the memory.search tool
    * (memory_callback.c:memory_action_search) AND the per-turn focus-
    * injection fact adapter (memory_focus_adapters.c:fact_adapter_query).
    * Both code paths feed the LLM verbatim (tool response vs. system-prompt
    * focus block) and share the same marginal-cosine fabrication surface,
    * so a single tunable keeps tool-time and injection-time semantics
    * aligned.  If a future use case justifies divergent thresholds (e.g.,
    * bench sweeps, A/B), split into `memory.search_score_floor` and
    * `focus_injection.fact_score_floor` rather than reaching past the helper
    * — the helper itself (memory_search_apply_score_floor) stays pure (no
    * config dep) precisely to preserve that seam. */
   float search_score_floor;
   /* Paraphrase-dedup gate at extraction time.  Replaces the old LIKE-based
    * fact-similar prefilter with an embedding-cosine check: if a newly
    * extracted fact's embedding scores >= paraphrase_dedup_threshold
    * against any existing fact in the user's cache, the new fact is merged
    * into the existing one (confidence bumped, provenance extended) instead
    * of being stored.  Set paraphrase_dedup_enabled = false to bypass the
    * gate entirely (kill switch when calibration is in flight or false
    * positives are observed).  Threshold band is conservative — false-
    * merge cost (lost distinct fact) > false-miss cost (a duplicate row),
    * so calibrate for precision. */
   bool paraphrase_dedup_enabled;
   float paraphrase_dedup_threshold;

   /* Note-extraction guard (Phase 9 of the notes/reference-text store).  When a
    * user files reference text via the `document_manage` save_note/save_text
    * tool, that text lives canonically in the document store.  This guard keeps
    * session-end memory extraction from RE-mining the same verbatim text into
    * semantic facts (double-storage + drift: edit the note → the fact goes
    * stale).  At extraction time it redacts (on copies, never the live history)
    * the filed bodies wherever they appear — the tool-call arguments and the
    * originating user message — plus the echoed bodies in `document_read` /
    * `document_search` tool RESULTS (which would otherwise re-inject the text in
    * a later extraction window).  Exact normalized-substring match only, so
    * zero false positives.  Default on; set false to disable. */
   bool note_extraction_guard;

   /* Extraction recovery — re-runs memory extraction on conversations whose
    * `last_extracted_msg_count` < `message_count` after the configured idle
    * window.  Catches sessions left unconsolidated by daemon crashes or
    * extraction failures.  Runs once at startup and on a recurring interval. */
   bool recovery_enabled;
   int recovery_idle_threshold_seconds;     /* Min seconds since updated_at (default 3600) */
   int recovery_max_attempts;               /* Per-conversation cap (default 2, 0=unlimited) */
   int recovery_recurring_interval_seconds; /* 0=startup only (default 86400 = 24h) */

   /* Per-turn focus injection — Phase 1 of Dynamic Context Injection.
    * Per-turn hook that retrieves relevant facts/entities/relations/
    * summaries/document chunks/calendar events/recent emails above a
    * relevance threshold and injects them as a budget-bounded "current
    * context" block.  Disabled by default until adapters land in 1c/1d
    * and the prompt-builder integration ships in 1e.  See
    * `docs/DYNAMIC_CONTEXT_INJECTION_DESIGN.md` §"Phase 1 — Per-Turn Focus". */
   focus_injection_config_t focus_injection;

   /* Unified cross-source `recall` tool — deep gather at a larger budget than
    * the per-turn focus injection above.  See recall_config_t. */
   recall_config_t recall;

   /* Phase 2 entity-merge auto-merge gate.  Fires after each extraction
    * completes, evaluates was_created entities against existing canonicals
    * via the resolver cascade, and routes by composite score:
    *   - composite >= auto_threshold      → soft-link written (silent)
    *   - composite >= review_threshold    → proposal queued for operator
    *                                        approval via WebUI Suggested-
    *                                        Merges panel
    *   - else                             → stays its own canonical
    * Operator approval gates the review band so lower review_threshold
    * trades precision for recall — false positives are cheap (one click
    * to reject) and false misses are expensive (duplicate persists).  The
    * auto_threshold should stay high (≥ 0.85) because auto-merges land
    * with no human in the loop. */
   bool entity_merge_enabled;
   float entity_merge_auto_threshold;
   float entity_merge_review_threshold;
} memory_config_t;

/* =============================================================================
 * Debug Configuration
 * ============================================================================= */
typedef struct {
   bool mic_record;                   /* Record raw microphone input */
   bool asr_record;                   /* Record ASR input audio */
   bool aec_record;                   /* Record AEC processed audio */
   char record_path[CONFIG_PATH_MAX]; /* Directory for debug recordings */
   bool silent_observe_test_endpoint; /* Expose POST /api/test/silent_observation (dev only) */
} debug_config_t;

/* =============================================================================
 * Paths Configuration
 * ============================================================================= */
typedef struct {
   char data_dir[CONFIG_PATH_MAX]; /* Data directory for databases (default: ~/.local/share/dawn) */
   char music_dir[CONFIG_PATH_MAX]; /* Music library location */
} paths_config_t;

/* =============================================================================
 * Scheduler Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;                /* Master enable/disable */
   int default_snooze_minutes;  /* Default snooze duration */
   int max_snooze_count;        /* Auto-cancel after this many snoozes */
   int max_events_per_user;     /* Prevent runaway event creation per user */
   int max_events_total;        /* Hard cap across all users */
   bool missed_event_recovery;  /* Handle missed events on startup */
   char missed_task_policy[16]; /* "skip" or "execute" */
   int missed_task_max_age_sec; /* Skip tasks older than this even if policy=execute */
   int alarm_timeout_sec;       /* Stop ringing after this (hard max: 300) */
   int alarm_volume;            /* Alarm sound volume (0-100) */
   int event_retention_days;    /* Clean up fired/cancelled/missed events after N days */
   /* When a briefing fires, TTS routes by source_client_type.  Voice-created
    * briefings (LOCAL/DAP2) always speak — the user asked aloud, they expect
    * to hear it.  WebUI-created briefings default to silent (the conversation
    * IS the artifact) unless this flag is on. */
   bool briefing_speak_aloud_on_webui_source;
} scheduler_config_t;

/* =============================================================================
 * Background Jobs Configuration
 *
 * A background job IS a parented conversation run in a separate text-only
 * job-session pool (never the audio-bound interactive array).  These caps are
 * LLM-resource-bound (provider concurrency) + DoS guards; job creation/listing/
 * cancel is conversational (the `job` tool + WebUI), never config.  See
 * docs/BACKGROUND_JOBS_DESIGN.md §7.
 * ============================================================================= */
typedef struct {
   bool enabled;                   /* Master enable/disable for background jobs */
   int max_concurrent_local;       /* Concurrent jobs on a local (GPU) LLM — keep low */
   int max_concurrent_cloud;       /* Concurrent jobs on a cloud LLM — parallelism is cheap */
   int max_active_jobs;            /* GLOBAL running ceiling across all users (pool size) */
   int max_jobs_per_user;          /* Per-user concurrent running cap */
   int max_queued_per_user;        /* Per-user bounded backlog; reject beyond (no silent enqueue) */
   int monitor_followups_per_tick; /* Bound heartbeat work so the voice loop isn't degraded */
   int max_spawn_depth;            /* Tree depth cap (exercised in Phase 3) */
   int max_children_per_tree;      /* Active children per parent (Phase 3) */
   int max_reinvokes_per_tree;     /* Runaway guard: v1 uses it as the per-job reinvoke
                                    * FAILURE-ATTEMPT cap (bump at attempt; force-fire past it);
                                    * true per-tree cumulative accounting lands with trees. */
   int max_concurrent_reinvokes;   /* Global cap on in-flight reinvoke_parent workers */
   int max_runtime_sec;            /* Per-job reap (still fires follow-up on timeout) */
   int event_chunk_cap;            /* Head+tail truncation for event payloads (Phase 2) */
} jobs_config_t;

/* =============================================================================
 * Music Configuration
 * ============================================================================= */

/** Plex Media Server connection settings ([music.plex] section) */
typedef struct {
   char host[CONFIG_PATH_MAX]; /* Plex server IP or hostname */
   int port;                   /* Plex server port (default: 32400) */
   int music_section_id;       /* 0 = auto-discover from /library/sections */
   bool ssl;                   /* Use HTTPS for Plex API calls */
   bool ssl_verify;            /* Verify TLS certificates (default: true) */
   char client_identifier[64]; /* Auto-generated UUID on first run */
} plex_config_t;

typedef struct {
   int scan_interval_minutes; /* Minutes between rescans (0 = disabled, default: 60) */

   /* Plex Media Server settings (music.plex section) */
   plex_config_t plex;

   /* Streaming settings (music.streaming section) */
   bool streaming_enabled;         /* Enable WebUI music streaming (default: true) */
   char streaming_quality[16];     /* Default quality: voice/standard/high/hifi */
   char streaming_bitrate_mode[8]; /* vbr or cbr */
} music_config_t;

/* =============================================================================
 * Calendar Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;                   /* Master enable/disable (default: true) */
   int sync_interval_sec;          /* Background sync interval (default: 900 = 15 min) */
   int cache_past_days;            /* Days of past events to cache (default: 30) */
   int cache_future_days;          /* Days of future events to cache (default: 365) */
   int default_event_duration_min; /* Default event duration in minutes (default: 60) */
} calendar_config_t;

/* =============================================================================
 * Messaging Channels Configuration
 *
 * Runtime knobs for the messaging-channels engine (Telegram / Discord /
 * Slack / SMS).  Driver-specific tokens live in secrets.toml; this
 * section covers behavior tunables.  See
 * docs/MESSAGING_CHANNELS_DESIGN.md.
 * ============================================================================= */
typedef struct {
   /* SMS-only: window after the LAST LLM-bound exchange during which
    * subsequent SMS from the same linked sender bypass the wake-word
    * gate and route directly to the LLM.  Outside the window an SMS
    * without a wake-word falls through to existing MIRAGE HUD + ctx
    * inject behavior.  Forever-conversation thread (the conv_id
    * binding on messaging_channels) persists across windows — this
    * only controls LLM routing, not conversation lifetime.
    *
    * Telegram/Discord/Slack are LLM-exclusive (every linked-sender
    * message routes to LLM), so this knob doesn't apply there.
    *
    * Default 600 (10 minutes).  Set 0 to disable (every SMS requires
    * an explicit wake-word). */
   int sms_active_window_sec;
} messaging_config_t;

/* =============================================================================
 * Secrets Configuration (loaded separately from secrets.toml)
 * ============================================================================= */
typedef struct {
   char openai_api_key[CONFIG_API_KEY_MAX];
   char claude_api_key[CONFIG_API_KEY_MAX];
   char gemini_api_key[CONFIG_API_KEY_MAX];
   char openrouter_api_key[CONFIG_API_KEY_MAX]; /* OpenRouter gateway key (sk-or-...) */
   char mqtt_username[CONFIG_CREDENTIAL_MAX];
   char mqtt_password[CONFIG_CREDENTIAL_MAX];

   /* Pre-shared key for satellite registration (empty = open registration)
    * 32-byte hex = 64 chars + null = 65 bytes minimum */
   char satellite_registration_key[CONFIG_API_KEY_MAX];

   /* Plex Media Server authentication token */
   char plex_token[CONFIG_API_KEY_MAX];

   /* Embedding API key (OpenAI provider; falls back to openai_api_key if empty) */
   char embedding_api_key[CONFIG_API_KEY_MAX];

   /* Home Assistant Long-Lived Access Token */
   char home_assistant_token[CONFIG_API_KEY_MAX];

   /* Google OAuth 2.0 (Calendar and Email) */
   char google_client_id[CONFIG_API_KEY_MAX];
   char google_client_secret[CONFIG_API_KEY_MAX];
   char google_redirect_url[CONFIG_PATH_MAX]; /* e.g. https://myhost:3000/oauth/callback */

   /* Service token for machine-to-machine image API access (MIRAGE, etc.) */
   char service_token[CONFIG_API_KEY_MAX];

   /* Tavily API key (LLM-optimized search + URL extract; opt-in alternative
    * to SearXNG + FlareSolverr). https://tavily.com — free tier 1000/mo. */
   char tavily_api_key[CONFIG_API_KEY_MAX];

   /* Telegram Bot API token (from @BotFather).  When empty, the
    * Telegram messaging driver is not registered.  See
    * docs/MESSAGING_CHANNELS_DESIGN.md §8 (Telegram). */
   char telegram_bot_token[CONFIG_API_KEY_MAX];

   /* Discord bot token (from Developer Portal → Bot).  When empty, the
    * Discord messaging driver is not registered.  v1 is DM-only and
    * requires the privileged MESSAGE_CONTENT intent.  See
    * docs/MESSAGING_CHANNELS_DESIGN.md §8 (Discord). */
   char discord_bot_token[CONFIG_API_KEY_MAX];

   /* Slack App-level token (xapp-...) used by the Socket Mode driver to
    * open the outbound WebSocket via apps.connections.open.  When empty
    * (or slack_bot_token empty), the Slack messaging driver is not
    * registered.  v1 is DM-only, single-workspace install.  See
    * docs/MESSAGING_CHANNELS_DESIGN.md §8 (Slack). */
   char slack_app_token[CONFIG_API_KEY_MAX];

   /* Slack Bot User OAuth token (xoxb-...) used for chat.postMessage
    * REST sends.  Required alongside slack_app_token.  Scopes required:
    * chat:write, im:history, im:read, app_mentions:read. */
   char slack_bot_token[CONFIG_API_KEY_MAX];
} secrets_config_t;

/* =============================================================================
 * OTA (over-the-air satellite updates) Configuration
 * ============================================================================= */
typedef struct {
   bool enabled;                      /* Master switch for serving OTA updates */
   char release_dir[CONFIG_PATH_MAX]; /* Root of signed release artifacts */
   int download_token_ttl_sec;        /* One-time image-download token lifetime (s) */
   bool require_tls;                  /* Reject OTA control/download over plaintext */
} ota_config_t;

/* =============================================================================
 * MCP bridge (coding-harness) — [mcp] + [[mcp.server]]
 * ============================================================================= */
#define MCP_SERVERS_MAX 16
#define MCP_SERVER_ALIAS_MAX 64

typedef struct {
   char alias[MCP_SERVER_ALIAS_MAX]; /* server id, used as a tool-name prefix */
   char url[CONFIG_PATH_MAX];        /* SSE endpoint URL */
   char transport[16];               /* "http+sse" (Phase 1) */
   bool enabled;
   char capabilities[16]; /* "dangerous" (default) | "read_only" */
   int request_timeout_seconds;
   int idle_close_seconds;
   bool tls_verify; /* false requires [mcp] dev_mode = true */
   /* Bearer token: env-var name is the Phase-1 source (auth_bearer_env). The
    * secrets.toml key is reserved — resolving it needs a generic by-name secret
    * lookup that the fixed-field secrets parser does not yet provide. */
   char auth_bearer_env[CONFIG_NAME_MAX];
   char auth_bearer_token_secret[CONFIG_NAME_MAX];
} mcp_server_config_t;

typedef struct {
   bool enabled;
   bool dev_mode; /* gate: required true to permit any tls_verify = false server */
   int server_count;
   mcp_server_config_t servers[MCP_SERVERS_MAX];
} mcp_config_t;

/* =============================================================================
 * Code projects (coding harness) — [code_projects]
 * ============================================================================= */
#define CODE_PROJECTS_MAX_LOCAL_ROOTS 8

typedef struct {
   bool enabled;
   char source_root[CONFIG_PATH_MAX]; /* where repos are cloned */
   char default_index_mode[16];       /* "full" */
   bool default_global;
   char import_user_required[16]; /* "" or "admin" */
   int max_repo_size_mb;
   int max_file_count;
   int max_path_depth;
   int clone_depth;                /* 0 = full, 1 = shallow */
   char allowed_host_pattern[256]; /* POSIX ERE for the clone host allowlist */
   char default_active[64];        /* fallback active project name */
   /* Absolute dir prefixes a link-local repo may live under. Empty = link-local
    * disabled. This is the content-exposure boundary (indexed file CONTENTS reach
    * the LLM); must stay in sync with the cbm-mcp.service BindReadOnlyPaths and
    * must not contain secret-bearing trees. */
   char allowed_local_roots[CODE_PROJECTS_MAX_LOCAL_ROOTS][CONFIG_PATH_MAX];
   int allowed_local_roots_count;
} code_projects_config_t;

/* =============================================================================
 * Proactive attention (SAGE) — [attention]
 * -----------------------------------------------------------------------------
 * Operator scalars only.  The watch RULES are per-user database rows managed at
 * runtime by the `attention` LLM tool and the WebUI Watches panel (the JARVIS
 * conversational-control principle) — never listed here.
 * ============================================================================= */
typedef struct {
   bool enabled;              /* master switch (evaluation + delivery) */
   int max_alerts_per_hour;   /* global spoken-alert budget */
   bool inject_into_sessions; /* also seed active-conversation LLM context (P0: off) */
   bool judge_enabled;        /* P2 LLM judge (parsed, ignored in P0) */
   double judge_threshold;    /* P2 score floor 1.0-5.0 */
   char quiet_hours[16];      /* P1 quiet-hours window, e.g. "23:00-07:00" */
} attention_config_t;

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
   documents_config_t documents;
   vision_config_t vision;
   memory_config_t memory;
   shutdown_config_t shutdown;
   debug_config_t debug;
   paths_config_t paths;
   music_config_t music;
   scheduler_config_t scheduler;
   jobs_config_t jobs;
   calendar_config_t calendar;
   messaging_config_t messaging;
   ota_config_t ota;
   mcp_config_t mcp;
   code_projects_config_t code_projects;
   attention_config_t attention;
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
 * @brief Clamp [jobs] caps to their safe bounds.
 *
 * Shared by the TOML parse path and the WebUI settings POST handler so the two
 * entry points cannot drift — a value arriving over the wire is bounded exactly
 * like one read from dawn.toml (max_active_jobs sizes the job-session pool
 * array, so an out-of-range value must never reach the allocation).
 *
 * @param config Jobs config to clamp in place (NULL-safe).
 */
void config_clamp_jobs(jobs_config_t *config);

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
