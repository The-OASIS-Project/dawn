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

#ifndef LLM_INTERFACE_H
#define LLM_INTERFACE_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

/* Default cloud LLM base URLs (overridable via llm.cloud.endpoint).  Shared so
 * the auxiliary resolvers (compaction/extraction/silent-observe) use the same
 * constants as the main path instead of duplicating literals. */
#define CLOUDAI_URL "https://api.openai.com"
#define CLAUDE_URL "https://api.anthropic.com"
#define GEMINI_URL "https://generativelanguage.googleapis.com/v1beta/openai"
/* Base WITHOUT the /v1 — the chat-completions path "/v1/chat/completions"
 * (OPENAI_CHAT_ENDPOINT) is appended downstream, matching OpenAI's bare host.
 * Full URL resolves to https://openrouter.ai/api/v1/chat/completions. */
#define OPENROUTER_URL "https://openrouter.ai/api"

/**
 * @brief LLM call mode
 *
 * Selects which call path is invoked.  Currently only the silent-observe path
 * branches on this enum; the chat-completion path is the implicit default and
 * does not dispatch on this value.
 *
 * Defined here so that future call paths can be enumerated without churn at
 * call sites.
 */
typedef enum {
   LLM_CALL_CHAT_COMPLETION, /**< User-facing chat completion (streaming or one-shot) */
   LLM_CALL_SILENT_OBSERVE,  /**< Background observation — schema-validated, no tools, no TTS */
} llm_call_mode_t;

/**
 * @brief Length of the source-category string in silent_observe_response_t
 *
 * The category is one of a fixed allowlist of 10 values; 32 bytes is comfortable
 * headroom and aligns with neighboring fixed-size buffers in this file.
 */
#define LLM_SILENT_OBSERVE_CATEGORY_MAX 32

/**
 * @brief Length of the note string in silent_observe_response_t
 *
 * Capped at 256 chars per the design spec — longer notes are truncated and the
 * call is logged as a schema-validation anomaly.
 */
#define LLM_SILENT_OBSERVE_NOTE_MAX 256

/**
 * @brief Strict response schema for silent observations
 *
 * Populated only on a SUCCESS return from llm_silent_observe().  Any of:
 * malformed JSON, missing field, category outside the fixed allowlist, note
 * length over LLM_SILENT_OBSERVE_NOTE_MAX, blocked-pattern match on input or
 * note, unexpected tool_calls in the LLM response → call returns FAILURE and
 * leaves *out untouched.
 */
typedef struct {
   bool ack;                                       /**< Always true on a SUCCESS return */
   char category[LLM_SILENT_OBSERVE_CATEGORY_MAX]; /**< One of the 10 allowlisted values */
   char note[LLM_SILENT_OBSERVE_NOTE_MAX];         /**< LLM-generated note, filter-checked */
} silent_observe_response_t;

/**
 * @brief Cloud provider types
 *
 * Automatically detected based on API keys in secrets.toml.
 * If both providers are configured, provider can be selected via
 * --cloud-provider command-line argument or dawn.toml config.
 */
typedef enum {
   CLOUD_PROVIDER_OPENAI,     /**< OpenAI (GPT models) */
   CLOUD_PROVIDER_CLAUDE,     /**< Anthropic Claude models */
   CLOUD_PROVIDER_GEMINI,     /**< Google Gemini models (OpenAI-compatible API) */
   CLOUD_PROVIDER_OPENROUTER, /**< OpenRouter gateway (OpenAI-compatible, many vendors) */
   CLOUD_PROVIDER_NONE        /**< No cloud provider configured */
   /* NOTE: append new providers BEFORE _NONE to preserve existing ordinals.
    * The enum is never serialized as an integer (only as strings via
    * cloud_provider_to_string), so ordinal stability is for in-memory state only. */
} cloud_provider_t;

/**
 * @brief LLM type (local vs cloud)
 */
typedef enum {
   LLM_LOCAL,    /**< Local LLM server (e.g., llama.cpp) */
   LLM_CLOUD,    /**< Cloud LLM provider (OpenAI/Claude/Gemini/OpenRouter) */
   LLM_UNDEFINED /**< Not yet initialized / inherit from global */
} llm_type_t;

/** Maximum length for LLM model names */
#define LLM_MODEL_NAME_MAX 64

/** CURL connect timeout in milliseconds (TCP + TLS handshake) */
#define LLM_CONNECT_TIMEOUT_MS 10000L

/** Maximum length for tool mode strings */
#define LLM_TOOL_MODE_MAX 16

/** Maximum length for thinking mode strings */
#define LLM_THINKING_MODE_MAX 16

/**
 * @brief Per-session LLM configuration
 *
 * Each session (WebUI, DAP, local) owns its own LLM settings.
 * Sessions are initialized with a copy of defaults at creation time.
 * Changes to one session's config do not affect other sessions.
 */
typedef struct {
   llm_type_t type;                              /**< LLM type (local or cloud) */
   cloud_provider_t cloud_provider;              /**< Cloud provider (OpenAI, Claude, etc.) */
   char endpoint[128];                           /**< Endpoint URL (empty = use provider default) */
   char model[LLM_MODEL_NAME_MAX];               /**< Model name (empty = use provider default) */
   char tool_mode[LLM_TOOL_MODE_MAX];            /**< Tool mode: native, command_tags, disabled */
   char thinking_mode[LLM_THINKING_MODE_MAX];    /**< Thinking: disabled, auto, enabled */
   char reasoning_effort[LLM_THINKING_MODE_MAX]; /**< Reasoning effort: low, medium, high */
} session_llm_config_t;

/**
 * @brief Resolved LLM configuration for making requests
 *
 * Created by llm_resolve_config() by merging session overrides with global config.
 *
 * WARNING: Pointers reference internal/stack memory that may become invalid
 * after llm_resolve_config() returns. Callers MUST copy string fields to local
 * buffers immediately using LLM_COPY_MODEL_SAFE() before any function calls.
 */
typedef struct {
   llm_type_t type;                              /**< Resolved LLM type */
   cloud_provider_t cloud_provider;              /**< Resolved cloud provider */
   const char *endpoint;                         /**< Endpoint URL (not owned, may be dangling) */
   const char *api_key;                          /**< API key for cloud providers (not owned) */
   const char *model;                            /**< Model name; not owned / may be dangling,
                                                    EXCEPT when remapped to an OpenRouter slug it
                                                    aliases model_buf below (stable for the
                                                    struct's lifetime) */
   char model_buf[LLM_MODEL_NAME_MAX];           /**< Backs `model` when remapped to an
                                                    OpenRouter slug (stable, in-struct) */
   char tool_mode[LLM_TOOL_MODE_MAX];            /**< Tool mode: native, command_tags, disabled */
   char thinking_mode[LLM_THINKING_MODE_MAX];    /**< Thinking: disabled, auto, enabled */
   char reasoning_effort[LLM_THINKING_MODE_MAX]; /**< Reasoning effort: low, medium, high */
   int timeout_ms; /**< Per-request timeout (0 = use global default) */
} llm_resolved_config_t;

/**
 * @brief Safely copy a model name to a local buffer
 *
 * Use this macro immediately after llm_resolve_config() to copy string fields
 * that may become dangling pointers. The macro handles NULL and empty strings.
 *
 * @param dst    Destination buffer (char array)
 * @param src    Source string (may be NULL)
 *
 * Example:
 *   llm_resolved_config_t resolved;
 *   char model_buf[LLM_MODEL_NAME_MAX];
 *   llm_resolve_config(&session_config, &resolved);
 *   LLM_COPY_MODEL_SAFE(model_buf, resolved.model);
 */
#define LLM_COPY_MODEL_SAFE(dst, src)            \
   do {                                          \
      if ((src) && (src)[0] != '\0') {           \
         strncpy((dst), (src), sizeof(dst) - 1); \
         (dst)[sizeof(dst) - 1] = '\0';          \
      } else {                                   \
         (dst)[0] = '\0';                        \
      }                                          \
   } while (0)

/**
 * @brief Initialize the LLM system
 *
 * Detects available cloud providers based on API keys in secrets.toml.
 * If command-line override provided, validates and uses it.
 * If both providers available and no override, defaults to OpenAI.
 *
 * @param cloud_provider_override Optional provider override from command line
 *                                 ("openai", "claude", or NULL to auto-detect)
 */
void llm_init(const char *cloud_provider_override);

/**
 * @brief Re-detect available cloud providers at runtime
 *
 * Call this after API keys are updated (e.g., via WebUI) to refresh
 * provider availability without restarting. Safe to call at any time.
 *
 * @return 1 if at least one cloud provider is now available, 0 otherwise
 */
int llm_refresh_providers(void);

/**
 * @brief Callback function type for streaming text chunks from LLM
 *
 * Called for each incremental text chunk received from the LLM during streaming.
 * The text should be processed immediately (e.g., sent to TTS).
 *
 * @param chunk Incremental text chunk
 * @param userdata User-provided context pointer
 */
typedef void (*llm_text_chunk_callback)(const char *chunk, void *userdata);

/**
 * @brief Callback function type for complete sentences from streaming
 *
 * Called for each complete sentence extracted from the LLM stream.
 * Use this for TTS to ensure natural speech boundaries.
 *
 * @param sentence Complete sentence text
 * @param userdata User-provided context pointer
 */
typedef void (*llm_sentence_callback)(const char *sentence, void *userdata);

/**
 * @brief Get chat completion from configured LLM (non-streaming)
 *
 * Routes to appropriate provider based on current configuration.
 * Handles local/cloud fallback automatically on connection failure if allow_fallback is true.
 * Conversation history is always stored in OpenAI format internally,
 * but converted as needed for Claude API calls.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param allow_fallback If true, falls back to local LLM on cloud failure
 * @return Response text (caller must free), or NULL on error
 */
char *llm_chat_completion(struct json_object *conversation_history,
                          const char *input_text,
                          const char **vision_images,
                          const size_t *vision_image_sizes,
                          int vision_image_count,
                          bool allow_fallback);

/**
 * @brief Get chat completion from configured LLM with streaming
 *
 * Routes to appropriate provider based on current configuration.
 * Calls chunk_callback for each incremental text chunk as it arrives.
 * The complete accumulated response is returned when streaming completes.
 * Handles local/cloud fallback automatically on connection failure if allow_fallback is true.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param chunk_callback Function to call for each text chunk (NULL for non-streaming)
 * @param callback_userdata User context passed to chunk_callback
 * @param allow_fallback If true, falls back to local LLM on cloud failure
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming(struct json_object *conversation_history,
                                    const char *input_text,
                                    const char **vision_images,
                                    const size_t *vision_image_sizes,
                                    int vision_image_count,
                                    llm_text_chunk_callback chunk_callback,
                                    void *callback_userdata,
                                    bool allow_fallback);

/**
 * @brief Get chat completion with streaming and sentence-boundary buffering for TTS
 *
 * Similar to llm_chat_completion_streaming, but buffers chunks and sends complete
 * sentences to the callback. This ensures TTS receives natural speech boundaries
 * (sentences ending with ., !, ?, :) for better prosody and intonation.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param sentence_callback Function to call for each complete sentence
 * @param callback_userdata User context passed to sentence_callback
 * @param allow_fallback If true, falls back to local LLM on cloud failure
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming_tts(struct json_object *conversation_history,
                                        const char *input_text,
                                        const char **vision_images,
                                        const size_t *vision_image_sizes,
                                        int vision_image_count,
                                        llm_sentence_callback sentence_callback,
                                        void *callback_userdata,
                                        bool allow_fallback);

/**
 * @brief Switch between local and cloud LLM
 *
 * @param type LLM_LOCAL or LLM_CLOUD
 * @return 0 on success, non-zero on failure (e.g., API key not configured)
 */
int llm_set_type(llm_type_t type);

/**
 * @brief Get current LLM type
 *
 * @return Current LLM type (LLM_LOCAL, LLM_CLOUD, or LLM_UNDEFINED)
 */
llm_type_t llm_get_type(void);

/**
 * @brief Get current cloud provider name (for display/logging)
 *
 * @return String name of provider ("OpenAI", "Claude", "Gemini", "None")
 */
const char *llm_get_cloud_provider_name(void);

/**
 * @brief Convert cloud provider enum to lowercase string
 *
 * Use this instead of inline ternary chains for provider-to-string conversion.
 *
 * @param provider Cloud provider enum value
 * @return Lowercase string ("openai", "claude", "gemini", "none")
 */
const char *cloud_provider_to_string(cloud_provider_t provider);

/**
 * @brief Convert a provider string to an LLM type + cloud provider enum
 *
 * Inverse of cloud_provider_to_string, plus the LLM type. "local"/"ollama" map to
 * LLM_LOCAL + CLOUD_PROVIDER_NONE; "openai"/"claude"/"anthropic"/"gemini"/"openrouter"
 * map to LLM_CLOUD + the matching provider. Use instead of inline string->enum ladders.
 *
 * @param str          Provider string (lowercase)
 * @param[out] type_out     Set to LLM_LOCAL or LLM_CLOUD on SUCCESS
 * @param[out] provider_out Set to the cloud provider enum on SUCCESS (NONE for local)
 * @return SUCCESS on a recognized provider; FAILURE on NULL args or unknown string
 *         (outputs left untouched on FAILURE)
 */
int cloud_provider_from_string(const char *str,
                               llm_type_t *type_out,
                               cloud_provider_t *provider_out);

/**
 * @brief Set the cloud provider at runtime
 *
 * Switches the active cloud provider. Validates that the required API key
 * is available before switching. Updates the endpoint URL if currently in cloud mode.
 *
 * @param provider CLOUD_PROVIDER_OPENAI, CLOUD_PROVIDER_CLAUDE, CLOUD_PROVIDER_GEMINI,
 *                 or CLOUD_PROVIDER_OPENROUTER
 * @return 0 on success, 1 on failure (API key not configured)
 */
int llm_set_cloud_provider(cloud_provider_t provider);

/**
 * @brief Get current cloud provider enum value
 *
 * @return Current cloud provider (CLOUD_PROVIDER_OPENAI, CLOUD_PROVIDER_CLAUDE,
 * CLOUD_PROVIDER_GEMINI, CLOUD_PROVIDER_OPENROUTER, or CLOUD_PROVIDER_NONE)
 */
cloud_provider_t llm_get_cloud_provider(void);

/**
 * @brief Get current LLM model name (for display/logging)
 *
 * @return String name of model (e.g., "gpt-4o", "claude-sonnet-4-5-20250929")
 */
const char *llm_get_model_name(void);

/**
 * @brief Get the default OpenAI model name from config
 *
 * Returns the model name at openai_default_model_idx in the openai_models array.
 * Falls back to the first model if index is out of bounds or no models configured.
 *
 * @return Model name string (pointer to config memory, do not free)
 */
const char *llm_get_default_openai_model(void);

/**
 * @brief Get the default Claude model name from config
 *
 * Returns the model name at claude_default_model_idx in the claude_models array.
 * Falls back to the first model if index is out of bounds or no models configured.
 *
 * @return Model name string (pointer to config memory, do not free)
 */
const char *llm_get_default_claude_model(void);

/**
 * @brief Get the default Gemini model name from config
 *
 * Returns the model name at gemini_default_model_idx in the gemini_models array.
 * Falls back to the first model if index is out of bounds or no models configured.
 *
 * @return Model name string (pointer to config memory, do not free)
 */
const char *llm_get_default_gemini_model(void);

/**
 * @brief Get the default OpenRouter model name from config
 *
 * Returns the model name at openrouter_default_model_idx in the openrouter_models
 * array.  Falls back to the first model if index is out of bounds or no models
 * configured.
 *
 * @return Model name string (pointer to config memory, do not free)
 */
const char *llm_get_default_openrouter_model(void);

/**
 * @brief Resolve a (possibly bare) model name to an OpenRouter "vendor/model" slug.
 *
 * Under OpenRouter gateway mode, requests must carry a vendor-qualified slug
 * (e.g. "openai/gpt-5.5").  A bare id like "gpt-5.5" would otherwise be dropped and
 * silently replaced by the gateway default — a DIFFERENT vendor's model.  This maps
 * it correctly using the provider hint:
 *   - already "vendor/model" (contains '/')  -> copied through unchanged;
 *   - else prefer an exact match in the configured openrouter_models[] catalog whose
 *     tail equals @p bare_model (vendor-checked against the provider hint);
 *   - else synthesize "<vendor>/<bare_model>" from the provider prefix
 *     (openai/, anthropic/, google/).
 *
 * @param provider    Provider hint (CLOUD_PROVIDER_OPENAI/CLAUDE/GEMINI). Others
 *                    yield a catalog-only match (no synthesized prefix).
 * @param bare_model  Requested model name (may already be a slug). May be NULL.
 * @param out         Output buffer for the resolved slug.
 * @param out_len     Size of @p out.
 * @return true if a vendor/model slug was produced; false if unresolvable (no '/',
 *         no provider prefix, and no catalog tail match) — caller should surface an
 *         error rather than silently fall back to the gateway default.
 */
bool llm_openrouter_slug_for(cloud_provider_t provider,
                             const char *bare_model,
                             char *out,
                             size_t out_len);

/**
 * @brief Check internet connectivity to LLM endpoint
 *
 * @param url URL to check
 * @param timeout_seconds Timeout in seconds
 * @return 1 if reachable, 0 otherwise
 */
int llm_check_connection(const char *url, int timeout_seconds);

/* Thread-local last-error codes — set by the LLM provider layer, read by
 * callers to distinguish failure modes when a chat-completion returns NULL.
 * Every public llm_chat_completion* entry point resets the code to
 * LLM_ERR_NONE on entry so each call's outcome stands on its own. */
#define LLM_ERR_NONE 0
#define LLM_ERR_TRANSIENT_NETWORK 1 /* pre-flight check failed, host unreachable */

/**
 * @brief Get the last LLM error code for the calling thread.
 *
 * Useful for distinguishing "LLM endpoint unreachable" (transient, worth
 * retrying after a back-off) from "LLM returned empty / invalid response"
 * (likely a real extraction failure).  The memory recovery / reextract
 * orchestrator reads this after every llm_chat_completion_with_config call
 * to decide whether to roll back the extraction_attempts counter.
 *
 * @return LLM_ERR_NONE if the last call had no signalled error, or one of
 *         the LLM_ERR_* codes above.
 */
int llm_last_error(void);

/**
 * @brief Set the thread-local last-error code.
 *
 * Provider-layer use only — declared here because providers live in
 * separate translation units (llm_claude.c / llm_openai_chat_completions.c)
 * and need to set this from their pre-flight check failure paths.  Not
 * intended for callers above the LLM layer; use llm_last_error() instead.
 *
 * @param code One of the LLM_ERR_* codes
 */
void llm_set_last_error(int code);

/**
 * @brief Request interruption of current LLM transfer
 *
 * Sets a flag that will cause the next CURL progress callback to abort
 * the transfer. Safe to call from signal handlers.
 */
void llm_request_interrupt(void);

/**
 * @brief Clear the LLM interrupt flag
 *
 * Should be called after handling an interrupted LLM call.
 */
void llm_clear_interrupt(void);

/**
 * @brief Check if LLM interrupt was requested
 *
 * @return 1 if interrupt requested, 0 otherwise
 */
int llm_is_interrupt_requested(void);

/**
 * @brief Set thread-local cancel flag for per-session cancellation
 *
 * Call this before starting an LLM request to enable per-session cancellation.
 * The cancel flag should point to a session-owned atomic_bool that gets set to
 * true when cancellation is requested. Set to NULL after the request completes.
 *
 * Implementation note: The flag is cast internally to _Atomic bool* for proper
 * atomic reads. Callers should pass &session->disconnected (an atomic_bool).
 *
 * @param flag Pointer to session's cancel flag (atomic_bool cast to void*), or NULL
 */
void llm_set_cancel_flag(void *flag);

/**
 * @brief Set the per-session cancel flag AND whether the in-flight transfer also
 *        honors the global interrupt flag (foreground wake-word / Ctrl+C).
 *
 * A background/job transfer passes honor_global=false so a foreground wake word
 * cannot abort its HTTP call (which would misfile as a transient network error).
 * llm_set_cancel_flag(flag) is the honor_global=true foreground shorthand.
 *
 * @param flag         Session cancel flag (atomic_bool cast to void*), or NULL.
 * @param honor_global True to also break on the global interrupt flag.
 */
void llm_set_cancel_flag_ex(void *flag, bool honor_global);

/**
 * @brief Get current thread-local cancel flag
 *
 * @return Current cancel flag pointer, or NULL if not set
 */
void *llm_get_cancel_flag(void);

/**
 * @brief Check if OpenAI API key is available
 *
 * Checks runtime config (secrets.toml)
 *
 * @return true if API key is available, false otherwise
 */
bool llm_has_openai_key(void);

/**
 * @brief Check if Claude API key is available
 *
 * Checks runtime config (secrets.toml)
 *
 * @return true if API key is available, false otherwise
 */
bool llm_has_claude_key(void);

/**
 * @brief Check if Gemini API key is available
 *
 * Checks runtime config (secrets.toml)
 *
 * @return true if API key is available, false otherwise
 */
bool llm_has_gemini_key(void);

/**
 * @brief Check if an OpenRouter API key is available
 *
 * Checks runtime config (secrets.toml or OPENROUTER_API_KEY env)
 *
 * @return true if openrouter_api_key is configured, false otherwise
 */
bool llm_has_openrouter_key(void);

/**
 * @brief Whether OpenRouter gateway mode is active.
 *
 * Reads g_config.llm.cloud.use_openrouter.  When true, ALL cloud traffic
 * (main chat + auxiliary extraction/compaction/silent-observe/scheduler calls)
 * routes through OpenRouter.  This is the single source of truth for the
 * bool→CLOUD_PROVIDER_OPENROUTER conversion done in llm_init/llm_refresh_providers.
 *
 * @return true if the gateway is enabled in config
 */
bool llm_openrouter_gateway_enabled(void);

/**
 * @brief Rewrite an auxiliary resolver's cloud target to OpenRouter when the
 *        gateway is on.
 *
 * Used by the string-keyed auxiliary resolvers (compaction, extraction,
 * silent-observe, scheduler) that do not go through the session path.  When the
 * gateway is enabled and *provider indicates a cloud provider, this sets
 * *provider = CLOUD_PROVIDER_OPENROUTER, writes *endpoint = OPENROUTER base URL
 * UNCONDITIONALLY (callers must not rely on downstream endpoint fallback), and
 * sets *api_key to the OpenRouter key.  The model string is left untouched — the
 * caller's configured model (e.g. an "anthropic/..." OpenRouter ID) is preserved.
 *
 * Reads global config; not reentrant across a mid-flight config change (matches
 * every other resolver's convention).
 *
 * @param provider [in,out] provider enum to (possibly) rewrite
 * @param endpoint [out]    receives the OpenRouter base URL when applied; may be NULL (skipped)
 * @param api_key  [out]    receives the OpenRouter key when applied; may be NULL (skipped)
 * @return true if the override was applied, false otherwise (advisory — callers may ignore)
 */
bool llm_apply_openrouter_gateway(cloud_provider_t *provider,
                                  const char **endpoint,
                                  const char **api_key);

/**
 * @brief Auto-detect the first cloud provider with an available API key
 *
 * When the OpenRouter gateway is on, returns CLOUD_PROVIDER_OPENROUTER if its
 * key is present.  Otherwise priority: Claude > OpenAI > Gemini.
 *
 * @return The first available provider, or CLOUD_PROVIDER_NONE if no keys found
 */
cloud_provider_t llm_detect_available_provider(void);

/* ============================================================================
 * Per-Session LLM Configuration Support
 * ============================================================================ */

/**
 * @brief Resolve session LLM config to final request config
 *
 * Resolves session config to final configuration with endpoints and API keys.
 * If session config specifies invalid settings (e.g., provider without API key),
 * returns error.
 *
 * @param session_config Session's LLM config
 * @param resolved Output: resolved configuration for LLM call
 * @return 0 on success, 1 on error (invalid config)
 */
int llm_resolve_config(const session_llm_config_t *session_config, llm_resolved_config_t *resolved);

/**
 * @brief Get default LLM configuration from dawn.toml settings
 *
 * Returns a session_llm_config_t populated with the default settings
 * from the global configuration. Used to initialize new sessions.
 *
 * @param config Output: default configuration
 */
void llm_get_default_config(session_llm_config_t *config);

/**
 * @brief Chat completion with explicit configuration (non-streaming)
 *
 * Same as llm_chat_completion() but uses provided config instead of global.
 * Resets llm_last_error() to LLM_ERR_NONE on entry so callers reading the
 * code after a NULL return see only this call's outcome.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param config Resolved LLM configuration to use
 * @return Response text (caller must free), or NULL on error
 */
char *llm_chat_completion_with_config(struct json_object *conversation_history,
                                      const char *input_text,
                                      const char **vision_images,
                                      const size_t *vision_image_sizes,
                                      int vision_image_count,
                                      const llm_resolved_config_t *config);

/**
 * @brief Chat completion with explicit configuration (streaming)
 *
 * Same as llm_chat_completion_streaming() but uses provided config.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param chunk_callback Function to call for each text chunk
 * @param callback_userdata User context passed to chunk_callback
 * @param config Resolved LLM configuration to use
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming_with_config(struct json_object *conversation_history,
                                                const char *input_text,
                                                const char **vision_images,
                                                const size_t *vision_image_sizes,
                                                int vision_image_count,
                                                llm_text_chunk_callback chunk_callback,
                                                void *callback_userdata,
                                                const llm_resolved_config_t *config);

/**
 * @brief Chat completion with explicit configuration (streaming TTS)
 *
 * Same as llm_chat_completion_streaming_tts() but uses provided config.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param sentence_callback Function to call for each complete sentence
 * @param callback_userdata User context passed to sentence_callback
 * @param config Resolved LLM configuration to use
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming_tts_with_config(struct json_object *conversation_history,
                                                    const char *input_text,
                                                    const char **vision_images,
                                                    const size_t *vision_image_sizes,
                                                    int vision_image_count,
                                                    llm_sentence_callback sentence_callback,
                                                    void *callback_userdata,
                                                    const llm_resolved_config_t *config);

/**
 * @brief Get full resolved LLM config for current session
 *
 * Returns the complete resolved config including type, cloud_provider,
 * endpoint, and API key. Used to detect provider changes after switch_llm.
 *
 * @param config_out Output: resolved config (caller owns, do not free strings)
 * @return 0 on success, 1 on failure
 */
int llm_get_current_resolved_config(llm_resolved_config_t *config_out);

/**
 * @brief Get effective LLM timeout in milliseconds
 *
 * Returns the thread-local override if set (> 0), otherwise the global config value.
 * Used by llm_openai.c and llm_claude.c to avoid reading a global that may be
 * mutated by concurrent threads.
 */
int llm_get_effective_timeout_ms(void);

/**
 * @brief Set thread-local LLM timeout override
 *
 * @param timeout_ms Timeout in milliseconds (0 to clear override)
 */
void llm_set_timeout_override(int timeout_ms);

/* ============================================================================
 * Silent-Observe — hardened LLM call mode for background observations
 * ============================================================================
 *
 * Phase 0 of the Dynamic Context Injection design (docs/DYNAMIC_CONTEXT_INJECTION_DESIGN.md).
 *
 * Architectural invariants (verified in CI / by inspection):
 *   1. Non-streaming, single-shot — uses provider's non-streaming completion path
 *      with empty tools array.  No streaming, no sentence callback, no TTS.
 *   2. Never enters src/llm/llm_tool_loop.c.  Tool-call dispatch is
 *      architecturally inaccessible.  Any tool_calls in the response → reject.
 *   3. Never appends to conv_db_* or session_manager_append_* — silent calls
 *      are not part of any user-facing conversation history.
 *   4. Never invokes text_to_speech() directly or indirectly.
 *   5. Tool-call rejection lives at this entry point, not inside any provider
 *      implementation: tool_mode = "disabled" suppresses tools in the request,
 *      and any leakage in the response is caught by schema validation.
 *
 * Hardening (mandatory, not optional):
 *   - Input wrapped in OBSERVATION DATA delimiters (data, not instructions)
 *   - Schema-validated JSON response (ack/category/note)
 *   - memory_filter_check() on input AND on the LLM's output note
 *   - Audit-log every call with tag prefix "silent_observe:"
 */

/**
 * @brief Make a silent-observe LLM call
 *
 * Wraps @p input_text in data-marking delimiters, dispatches to the configured
 * silent-observe provider with a non-streaming, no-tools, no-thinking request,
 * validates the JSON response against silent_observe_response_t, and returns
 * the structured ack on success.
 *
 * Caller does NOT need to filter @p input_text first — this function calls
 * memory_filter_check() on both the input and the LLM's output note.
 *
 * @param input_text       Observation text to summarize (not NULL).
 * @param source_category  Caller-supplied category for audit logging.  Free-form;
 *                         distinct from the LLM-emitted category in the response.
 * @param user_id          Owning user (for audit logging; may be 0 for system-scoped
 *                         observations).
 * @param out              Output: structured response on SUCCESS.  Untouched on
 *                         FAILURE.
 * @return SUCCESS (0) on validated, accepted observation.  FAILURE (1) on any
 *         rejection path (filter match, schema fail, tool-call leakage, network
 *         error, missing config).
 */
int llm_silent_observe(const char *input_text,
                       const char *source_category,
                       int user_id,
                       silent_observe_response_t *out);

/**
 * @brief Listener callback for silent-observation events
 *
 * Layer-4 surfaces (e.g. WebUI) register a single listener via
 * llm_silent_observe_set_event_listener().  The silent-observe primitive
 * (Layer 2) invokes this callback for every observation outcome — accepted,
 * schema-failed, or filter-rejected — without taking a direct dependency on
 * Layer-4 code.
 *
 * @param category      Category — one of the allowlisted values, or
 *                      "filtered" / "rejected" for non-success paths.
 * @param note          LLM-generated note on accept; placeholder text on filter
 *                      match (does not expose the offending content).
 * @param user_id       Owning user (0 for system-scoped observations).
 * @param filter_match  True when the call was rejected by the injection filter.
 */
typedef void (*silent_observe_event_fn)(const char *category,
                                        const char *note,
                                        int user_id,
                                        bool filter_match);

/**
 * @brief Register a listener for silent-observation events
 *
 * Setting NULL clears the listener.  The listener is invoked synchronously from
 * the thread that called llm_silent_observe().  Listeners must not block.
 *
 * @param fn Callback to register, or NULL to clear.
 */
void llm_silent_observe_set_event_listener(silent_observe_event_fn fn);

/**
 * @brief Check whether a provider name is valid for silent-observe.
 *
 * Single source of truth for the silent-observe provider allowlist —
 * consumed by config_validate.c, webui_config.c, and the resolver inside
 * llm_silent_observe.c so the list cannot drift between them.
 *
 * @param provider Provider string to check (case-sensitive).
 * @return true if provider is one of the accepted values.
 */
bool llm_silent_observe_provider_is_valid(const char *provider);

#endif  // LLM_INTERFACE_H
