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

#ifndef LLM_CLAUDE_H
#define LLM_CLAUDE_H

#include <json-c/json.h>
#include <stddef.h>

/*
 * Anthropic Claude Configuration
 *
 * Model, API version, and behavior settings for Claude provider.
 * These are compile-time defaults; runtime config in dawn.toml takes precedence.
 */

/* Default model for Claude */
#define CLAUDE_MODEL "claude-sonnet-4-5-20250929"
/* Alternative models:
 * #define CLAUDE_MODEL "claude-haiku-4-5-20241022"    // Faster, cheaper
 * #define CLAUDE_MODEL "claude-opus-4-20250514"       // Most capable
 */

/* Claude API version header value */
#define CLAUDE_API_VERSION "2023-06-01"

/* Max tokens for completion */
#define CLAUDE_MAX_TOKENS 4096

/* API endpoint path */
#define CLAUDE_MESSAGES_ENDPOINT "/v1/messages"

/* Prompt caching settings (1 = enabled, 0 = disabled) */
#define CLAUDE_ENABLE_PROMPT_CACHING 1

/**
 * @brief Callback function type for streaming text chunks
 */
typedef void (*llm_claude_text_chunk_callback)(const char *chunk, void *userdata);

/**
 * @brief Claude chat completion (non-streaming)
 *
 * Handles Anthropic Claude API calls with automatic format conversion.
 * Conversation history is provided in OpenAI format and converted internally
 * to Claude's format. Supports vision API and prompt caching.
 *
 * @param conversation_history JSON array of messages (OpenAI format - will be converted)
 * @param input_text User input text
 * @param vision_image Optional base64 image for vision models (NULL if not used)
 * @param vision_image_size Image size in bytes (0 if not used)
 * @param base_url Base URL (should be https://api.anthropic.com)
 * @param api_key Anthropic API key (required)
 * @return Response text (caller must free), or NULL on error
 */
char *llm_claude_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 char *vision_image,
                                 size_t vision_image_size,
                                 const char *base_url,
                                 const char *api_key);

/**
 * @brief Claude chat completion with streaming
 *
 * Handles Anthropic Claude API calls with Server-Sent Events (SSE) streaming.
 * Calls chunk_callback for each incremental text chunk as it arrives.
 * Returns the complete accumulated response when streaming completes.
 *
 * @param conversation_history JSON array of messages (OpenAI format - will be converted)
 * @param input_text User input text
 * @param vision_image Optional base64 image for vision models (NULL if not used)
 * @param vision_image_size Image size in bytes (0 if not used)
 * @param base_url Base URL (should be https://api.anthropic.com)
 * @param api_key Anthropic API key (required)
 * @param chunk_callback Function to call for each text chunk
 * @param callback_userdata User context passed to chunk_callback
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_claude_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           char *vision_image,
                                           size_t vision_image_size,
                                           const char *base_url,
                                           const char *api_key,
                                           llm_claude_text_chunk_callback chunk_callback,
                                           void *callback_userdata);

#endif  // LLM_CLAUDE_H
