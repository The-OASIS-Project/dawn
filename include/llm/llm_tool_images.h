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
 * Images a tool returns (a camera capture): checked, stored and shown.
 *
 * Every tool image comes in through one place (llm_tools.c), which hands it
 * here.  The image must be a JPEG, PNG, GIF or WebP by its bytes (whatever it
 * claims) and at most LLM_TOOL_IMAGE_MAX_BYTES.  For a turn with a user, it is
 * stored in the image store (IMAGE_SOURCE_CAPTURE, owner-only) as an unbound
 * image, owned by the user the turn's conversation is saved under; the tool
 * row that names it binds it when it is saved (auth_db_messages.h), and the
 * store reclaims it otherwise.  A guest's (or a call with no session's)
 * image is kept in memory only: that history never becomes a conversation.
 *
 * The result then carries the image in its content: [text part, image
 * parts], each image part marked with the stored id (IMAGE_PART_ID_KEY), on
 * every history format.  The live part is built from the bytes the turn
 * stored, which it still holds (no re-read of the file), in the form a
 * reload builds from the file (image_rehydrate_parts): a live request and a
 * reloaded one carry the same bytes.
 */

#ifndef LLM_TOOL_IMAGES_H
#define LLM_TOOL_IMAGES_H

#include <stdbool.h>

#include "llm/llm_capabilities.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Largest image a tool may return, in bytes (decoded); larger is refused. */
#define LLM_TOOL_IMAGE_MAX_BYTES (1536 * 1024)

/**
 * @brief Take a tool's image (@p base64, as the tool returned it; taken: the
 *        call frees it or keeps it in @p result)
 *
 * On success @p result holds it: vision_image (base64), and when stored,
 * vision_image_id and vision_image_owner.  Refused (not an image, too large),
 * or not kept (the store failed for a turn that would save it), @p result's
 * text says why and it holds no image.
 *
 * @return true when @p result holds the image
 */
bool llm_tool_images_ingest(char *base64, tool_result_t *result);

/**
 * @brief The content a result's history message carries: its text alone (a
 *        string) when it holds no image, else [text part, image_url parts],
 *        the same on every history format (llm_tool_images_render.h)
 * @return New object (caller owns), or NULL on out of memory
 */
struct json_object *llm_tool_images_result_content(const tool_result_t *result);

/** What a refused capture's result says (its request would pass the limit).
 *  It promises nothing: whether a later seam can summarize earlier images
 *  depends on where they sit. */
#define LLM_TOOL_IMAGES_REFUSED_TEXT \
   "capture refused: this conversation already holds as many images as one request can carry"

/** A model whose window is at most this is a "_200k" one (Anthropic counts its
 *  images per request lower). */
#define LLM_TOOL_IMAGES_SMALL_WINDOW 200000

/** The limit when models.toml gives none for a model's vendor. */
#define LLM_TOOL_IMAGES_DEFAULT_COUNT 20
#define LLM_TOOL_IMAGES_DEFAULT_BYTES ((int64_t)16000000)

/**
 * @brief The images one request to @p model may carry (models.toml
 *        [max_request_images], by its vendor), into @p out
 * @return false when models.toml gives none (@p out is the default then)
 */
bool llm_tool_images_request_limit(llm_type_t type,
                                   cloud_provider_t provider,
                                   const char *model,
                                   llm_image_limit_t *out);

/**
 * @brief Refuse each image in @p results that would take the next request
 *        past @p limit, in order, counting @p history's images first
 *
 * A turn's tool loop has no seam to compact at, so an image past the limit is
 * refused for this turn (its result says LLM_TOOL_IMAGES_REFUSED_TEXT, a
 * stored one is deleted: nothing names it), and a later seam compacts the
 * turns holding them once they are past what it keeps
 * (llm_tool_images_history_over).
 *
 * @return The number refused
 */
int llm_tool_images_cap_batch(struct json_object *history,
                              tool_result_list_t *results,
                              const llm_image_limit_t *limit);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_IMAGES_H */
