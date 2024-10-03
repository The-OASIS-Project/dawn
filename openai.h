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

#define OPENAI_URL	"https://api.openai.com"
//#define OPENAI_URL	"http://127.0.0.1:8080"

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
