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

#ifndef OPENAI_H
#define OPENAI_H

#define CLOUDAI_URL	"https://api.openai.com"
#define LOCALAI_URL	"http://127.0.0.1:8080"

/**
 * @brief Enumeration for different LLM (Large Language Model) types.
 *
 * This enumeration defines two types of LLM:
 * - CLOUD_LLM: Represents a cloud-based large language model.
 * - LOCAL_LLM: Represents a local instance of a large language model.
 */
typedef enum {
   CLOUD_LLM,  /**< Cloud-based large language model */
   LOCAL_LLM   /**< Local large language model */
} llm_t;

/**
 * @brief Sets the LLM (Large Language Model) type to either cloud or local.
 *
 * This function changes the LLM type based on the input parameter. It updates the URL
 * in the `llm_url` buffer based on whether the user selects the cloud-based or local-based LLM.
 * It also provides feedback using the `text_to_speech()` function.
 *
 * @param type The LLM type to be set (either CLOUD_LLM or LOCAL_LLM).
 */
void setLLM(llm_t type);

/**
 * @brief Check internet connection with timeout.
 *
 * This function checks the availability of an internet connection by attempting to
 * establish a connection to the specified URL within a given timeout period.
 *
 * @param url The URL to check for connection.
 * @param timeout_seconds The timeout value in seconds for the connection attempt.
 * @return 1 if a connection is successfully established within the timeout, 0 otherwise.
 *
 * @note This function uses non-blocking socket techniques and the select() function
 *       to provide a timeout mechanism for the connection attempt.
 */
int checkInternetConnectionWithTimeout(const char *url, int timeout_seconds);

/**
 * @brief Get a response using the GPT-based conversational model.
 *
 * This function takes the conversation history and input text, and generates a response
 * using the GPT-based conversational model. It does not modify the provided inputs.
 *
 * @param conversation_history A JSON object representing the conversation history.
 * @param input_text The input text for which a response is to be generated.
 * @return A pointer to the response text generated by the model. The memory for the response
 *         text is allocated by the function and must be freed by the caller.
 */
char *getGptResponse(struct json_object *conversation_history, const char *input_text,
                     char *vision_ai_image, size_t vision_ai_image_size);

#endif // OPENAI_H
