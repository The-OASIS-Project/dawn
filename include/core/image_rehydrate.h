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
 * Image markers in stored messages: an image turn is saved as its text plus
 * [IMAGE:img_id] markers, and every load that rebuilds a model's request turns
 * them back into the image parts the model was sent.  Also the marker-id
 * parse the retention-bump and cascade-delete image-lifecycle paths share.
 */

#ifndef CORE_IMAGE_REHYDRATE_H
#define CORE_IMAGE_REHYDRATE_H

#include "image_store.h" /* IMAGE_ID_LEN */

struct json_object;

#ifdef __cplusplus
extern "C" {
#endif

/* Defensive upper bound on total rehydrated image bytes per conversation restore.
 * The signed-off policy is "rehydrate ALL images"; this is purely a crash backstop
 * so a pathological conversation degrades to "[earlier image omitted]" instead of
 * OOM-killing the daemon.  Set far above any realistic live-session image peak. */
#define IMAGE_REHYDRATE_MAX_BYTES ((size_t)256 * 1024 * 1024)

/* Companion crash backstop on the number of image parts materialized per restored
 * message — bounds the part array independently of the byte ceiling (e.g. many tiny
 * images).  Far above WEBUI_MAX_VISION_IMAGES_CAP (the per-turn upload cap in
 * include/webui/webui_server.h); if that cap is ever raised past this value, bump this
 * one too or restored messages will degrade to "[earlier image omitted]". */
#define IMAGE_REHYDRATE_MAX_IMAGES 64

/**
 * @brief Collect valid image IDs from [IMAGE:img_id] markers in @p content.
 *
 * Legacy inline [IMAGE:data:...] markers (no stored id) are skipped. Each id is
 * validated with image_store_validate_id (the single authoritative validator).
 * Writes up to @p max NUL-terminated ids into @p ids_out[i] (each IMAGE_ID_LEN
 * bytes) and sets *@p count_out. Used by the retention-bump and cascade-delete paths.
 *
 * @param content   Stored message content to scan.
 * @param ids_out   Caller-allocated [max][IMAGE_ID_LEN] buffer.
 * @param max       Capacity of @p ids_out.
 * @param count_out [out, optional] Set to the number of ids collected (>= 0), or 0
 *                  on bad args; pass NULL to ignore.
 * @return SUCCESS, or FAILURE on bad args.
 */
int image_marker_collect_ids(const char *content,
                             char ids_out[][IMAGE_ID_LEN],
                             int max,
                             int *count_out);

/**
 * @brief Build a stored message for the LLM context, rehydrating image markers.
 *
 * If @p content has no [IMAGE:...] markers, returns a plain text message.
 * Otherwise builds an OpenAI multi-part message: one text part (prose, markers
 * removed) followed by image_url parts. Each [IMAGE:img_id] is OWNER-CHECKED
 * (image_store_get_metadata → require md.user_id == @p user_id, independent of
 * image source) then fetched and base64-encoded with its real mime. A missing /
 * non-owned / unreadable image, or one past IMAGE_REHYDRATE_MAX_BYTES, degrades to
 * an inline "[image no longer available]" / "[earlier image omitted]" note — a
 * missing image never fails the message. Legacy [IMAGE:data:...] markers pass
 * through inline.  Touches no session, so a restore can build a whole history
 * off-session and install it in one step.
 *
 * @param user_id Authenticated user id (ownership boundary; never 0).
 * @param role    Message role.
 * @param content Stored message content.
 * @return New {role, content} message (caller owns), or NULL on NULL args / OOM.
 */
struct json_object *image_rehydrate_message(int user_id, const char *role, const char *content);

/**
 * @brief Build the persisted form of an image turn: @p text + one `\n[IMAGE:<id>]`
 *        marker per id.
 *
 * The server-authoritative build-side twin of image_marker_collect_ids' parse: the
 * daemon now persists user image turns itself (no client save), so it constructs
 * the same marker grammar the browser used to (`\n[IMAGE:` + id + `]`, one per
 * image, appended after the prose).  Each id is validated with
 * image_store_validate_id; invalid ids are skipped.  Pure string assembly — no
 * image-store fetch — so the marker format lives only in this module.
 *
 * @param text  Clean user text (the prose half).
 * @param ids   Caller array of NUL-terminated image ids.
 * @param count Number of ids.
 * @return Heap string (caller frees) = text + markers, or a plain strdup(text) when
 *         no valid ids, or NULL on OOM / NULL text.
 */
char *image_marker_build_content(const char *text, const char ids[][IMAGE_ID_LEN], int count);

#ifdef __cplusplus
}
#endif

#endif /* CORE_IMAGE_REHYDRATE_H */
