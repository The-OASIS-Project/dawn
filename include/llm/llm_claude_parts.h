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
 * the project author(s).
 *
 * Claude user-message parts (tool results, images) read for other providers'
 * request shapes.
 */

#ifndef LLM_CLAUDE_PARTS_H
#define LLM_CLAUDE_PARTS_H

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/**
 * @brief A Claude tool_result part's text
 *
 * Its content as a string, or its text parts joined with newlines (other part
 * types, such as images, are not text and are left out).
 *
 * @return A new string (caller frees; "" when it has no text), or NULL on
 *         allocation failure
 */
char *llm_claude_tool_result_text(struct json_object *part);

/**
 * @brief A Claude base64 image part as a data URL ("data:<type>;base64,...")
 *
 * Only an image type every provider accepts (JPEG, PNG, GIF, WebP).
 *
 * @return A new string (caller frees), or NULL when the part isn't such an
 *         image or on allocation failure
 */
char *llm_claude_image_data_url(struct json_object *part);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CLAUDE_PARTS_H */
