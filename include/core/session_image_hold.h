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
 * Which unbound images a live session still holds: its unsaved history (and
 * what a compaction took out of it, waiting for the voice save) names them.
 * The image store's unbound sweep (image_store_reclaim_unbound) asks this,
 * through a weak symbol (the store sits a layer below sessions), before it
 * reclaims an unbound image past its grace, so a voice session or a job that
 * stays busy longer than the grace keeps its captures until it is saved.
 * Only the image owner's sessions count: an interactive one or a job's.
 */

#ifndef CORE_SESSION_IMAGE_HOLD_H
#define CORE_SESSION_IMAGE_HOLD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Most ids one call asks about (the image store's sweep batch). */
#define SESSION_IMAGES_HELD_MAX 100

/**
 * @brief Mark held[i] for each of @p ids whose owner, @p owners[i], has a live
 *        session (interactive or a job's) whose history names it
 *
 * Takes the session registry's lock (a snapshot, released), then each
 * session's history_mutex in turn; call with no other lock held (the image
 * store calls it with the auth_db lock released).  At most
 * SESSION_IMAGES_HELD_MAX ids are asked about.
 */
void session_images_held(const char *const ids[], const int owners[], int n, bool held[]);

/**
 * @brief Mark held[i] for each of @p ids that @p json names: an image part's
 *        stored id (IMAGE_PART_ID_KEY) or a saved row's images
 *        (LLM_HISTORY_ROW_IMAGES_KEY), at any depth
 * @return How many of @p ids are now marked
 */
int session_images_named(struct json_object *json, const char *const ids[], int n, bool held[]);

#ifdef __cplusplus
}
#endif

#endif /* CORE_SESSION_IMAGE_HOLD_H */
