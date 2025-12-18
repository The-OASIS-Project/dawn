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

/**
 * @brief Cloud provider types
 *
 * Automatically detected based on API keys in secrets.toml.
 * If both providers are configured, provider can be selected via
 * --cloud-provider command-line argument or dawn.toml config.
 */
typedef enum {
   CLOUD_PROVIDER_OPENAI, /**< OpenAI (GPT models) */
   CLOUD_PROVIDER_CLAUDE, /**< Anthropic Claude models */
   CLOUD_PROVIDER_NONE    /**< No cloud provider configured */
} cloud_provider_t;

/**
 * @brief LLM type (local vs cloud)
 */
typedef enum {
   LLM_LOCAL,    /**< Local LLM server (e.g., llama.cpp) */
   LLM_CLOUD,    /**< Cloud LLM provider (OpenAI or Claude) */
   LLM_UNDEFINED /**< Not yet initialized */
} llm_type_t;

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
 * Handles local/cloud fallback automatically on connection failure.
 * Conversation history is always stored in OpenAI format internally,
 * but converted as needed for Claude API calls.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_image Optional base64 image for vision models (NULL if not used)
 * @param vision_image_size Size of vision image data (0 if not used)
 * @return Response text (caller must free), or NULL on error
 */
char *llm_chat_completion(struct json_object *conversation_history,
                          const char *input_text,
                          char *vision_image,
                          size_t vision_image_size);

/**
 * @brief Get chat completion from configured LLM with streaming
 *
 * Routes to appropriate provider based on current configuration.
 * Calls chunk_callback for each incremental text chunk as it arrives.
 * The complete accumulated response is returned when streaming completes.
 * Handles local/cloud fallback automatically on connection failure.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_image Optional base64 image for vision models (NULL if not used)
 * @param vision_image_size Size of vision image data (0 if not used)
 * @param chunk_callback Function to call for each text chunk (NULL for non-streaming)
 * @param callback_userdata User context passed to chunk_callback
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming(struct json_object *conversation_history,
                                    const char *input_text,
                                    char *vision_image,
                                    size_t vision_image_size,
                                    llm_text_chunk_callback chunk_callback,
                                    void *callback_userdata);

/**
 * @brief Get chat completion with streaming and sentence-boundary buffering for TTS
 *
 * Similar to llm_chat_completion_streaming, but buffers chunks and sends complete
 * sentences to the callback. This ensures TTS receives natural speech boundaries
 * (sentences ending with ., !, ?, :) for better prosody and intonation.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_image Optional base64 image for vision models (NULL if not used)
 * @param vision_image_size Size of vision image data (0 if not used)
 * @param sentence_callback Function to call for each complete sentence
 * @param callback_userdata User context passed to sentence_callback
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming_tts(struct json_object *conversation_history,
                                        const char *input_text,
                                        char *vision_image,
                                        size_t vision_image_size,
                                        llm_sentence_callback sentence_callback,
                                        void *callback_userdata);

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
 * @return String name of provider ("OpenAI", "Claude", "None")
 */
const char *llm_get_cloud_provider_name(void);

/**
 * @brief Set the cloud provider at runtime
 *
 * Switches between OpenAI and Claude. Validates that the required API key
 * is available before switching. Updates the endpoint URL if currently in cloud mode.
 *
 * @param provider CLOUD_PROVIDER_OPENAI or CLOUD_PROVIDER_CLAUDE
 * @return 0 on success, 1 on failure (API key not configured)
 */
int llm_set_cloud_provider(cloud_provider_t provider);

/**
 * @brief Get current cloud provider enum value
 *
 * @return Current cloud provider (CLOUD_PROVIDER_OPENAI, CLOUD_PROVIDER_CLAUDE, or
 * CLOUD_PROVIDER_NONE)
 */
cloud_provider_t llm_get_cloud_provider(void);

/**
 * @brief Get current LLM model name (for display/logging)
 *
 * @return String name of model (e.g., "gpt-4o", "claude-sonnet-4-5-20250929")
 */
const char *llm_get_model_name(void);

/**
 * @brief Check internet connectivity to LLM endpoint
 *
 * @param url URL to check
 * @param timeout_seconds Timeout in seconds
 * @return 1 if reachable, 0 otherwise
 */
int llm_check_connection(const char *url, int timeout_seconds);

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

#endif  // LLM_INTERFACE_H
