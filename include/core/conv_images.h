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
 * The images a conversation owns, and what goes when it (or its user) does.
 * A conversation owns the images only it names and has bound: a tool row's
 * captures and a question's uploads (conversation_images, see
 * auth_db_messages.h); never an image a reply merely quotes, nor one another
 * conversation also names.  Every user-facing conversation delete removes
 * them through conv_images_delete_conversation(), and every account delete
 * purges the user's stores through conv_images_purge_user().
 */

#ifndef CORE_CONV_IMAGES_H
#define CORE_CONV_IMAGES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Delete a conversation and the images it owns, in one transaction
 *        (their files unlinked after it commits, with no lock held)
 *
 * Left alone: an unbound capture (a running turn, or a save to be retried,
 * holds it: the grace sweep reclaims it if not), an image another
 * conversation names, and anything a reply only quotes.
 *
 * @param user_id The owner, or 0 for any owner's (admin)
 * @return conv_db_delete_ex()'s: AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND,
 *         AUTH_DB_INVALID or AUTH_DB_FAILURE (nothing deleted)
 */
int conv_images_delete_conversation(int64_t conv_id, int user_id);

/**
 * @brief Purge everything a user's stores hold (images, document originals:
 *        rows + files), before the account is deleted
 *
 * The stores' rows go with the user by foreign key, their files don't; after
 * the user row is gone nothing can find them.
 * @return true when every store was purged
 */
bool conv_images_purge_user(int user_id);

#ifdef __cplusplus
}
#endif

#endif /* CORE_CONV_IMAGES_H */
