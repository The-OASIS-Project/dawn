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
 * Stable memory citation handles: the first time an item is injected into a
 * conversation it gets the next handle there ([M1], [M2], ...), and keeps it for
 * the conversation's life, across turns and reloads.
 */

#ifndef AUTH_DB_FOCUS_HANDLES_H
#define AUTH_DB_FOCUS_HANDLES_H

#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest source name and item id stored, in bytes (matching the focus
 * framework's buffers, less the terminator). */
#define CONV_FOCUS_SOURCE_MAX 31
#define CONV_FOCUS_ITEM_ID_MAX 63

/** One item to give a handle. */
typedef struct {
   const char *source;  /**< In: focus source name (e.g. "memory_fact") */
   const char *item_id; /**< In: opaque item key (e.g. "fact:123") */
   int handle;          /**< Out: the item's handle in the conversation */
   bool is_new;         /**< Out: true when assigned by this call */
} conv_focus_handle_t;

/**
 * Give each item its handle in the conversation: the existing one, or the next
 * unused one. All items are assigned in one transaction, so concurrent turns
 * never hand two items the same handle.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND (no such conversation for the
 *         user), AUTH_DB_INVALID (a missing or oversized name), or
 *         AUTH_DB_FAILURE (nothing assigned)
 */
int conv_db_focus_handles_assign(int64_t conv_id,
                                 int user_id,
                                 conv_focus_handle_t *items,
                                 int count);

/**
 * Store items with the handles they were already given (a conversation's first
 * turn numbers them before the conversation is saved). An item or handle the
 * conversation already has is left as it is.
 *
 * @param items In: source, item_id and handle of each; is_new is set when stored
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_INVALID, or AUTH_DB_FAILURE
 */
int conv_db_focus_handles_put(int64_t conv_id, int user_id, conv_focus_handle_t *items, int count);

/** Called once per stored handle, in handle order; return non-zero to stop. */
typedef int (*conv_focus_handle_cb_t)(const char *source,
                                      const char *item_id,
                                      int handle,
                                      void *ctx);

/**
 * Every handle a conversation has assigned, in handle order.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_focus_handles_load(int64_t conv_id, int user_id, conv_focus_handle_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_DB_FOCUS_HANDLES_H */
