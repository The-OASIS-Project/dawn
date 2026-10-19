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
 * A music player's play history: what it played, in order, like a browser's
 * back and forward list, so "previous" under shuffle goes back.  Pure: no
 * locks (the caller holds its own), no queue type (a caller's lookup turns a
 * path into a queue position).
 */

#ifndef MUSIC_NAV_H
#define MUSIC_NAV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Entries kept; the oldest is dropped past this */
#define MUSIC_HISTORY_MAX 50

/**
 * Finds @p path in the caller's queue: the copy nearest @p hint (the position
 * it had when recorded), or -1 when it isn't queued any more.
 */
typedef int (*music_nav_find_fn)(void *ctx, const char *path, int hint);

/**
 * Played tracks, oldest first.  Entries are kept as paths (queue positions
 * move as tracks are added and removed); entries after @c pos were played
 * and stepped back from, or planned to play next.
 */
typedef struct {
   char *path[MUSIC_HISTORY_MAX]; /**< Owned copies */
   int hint[MUSIC_HISTORY_MAX];   /**< Queue position when recorded */
   int len;                       /**< Entries in use */
   int pos;                       /**< Current entry, -1 when empty */
   uint32_t epoch;                /**< Queue replace epoch the entries belong to */
} music_history_t;

/** Empty history (a zeroed struct is not: pos must be -1) */
void music_history_init(music_history_t *h);

/** Drop every entry */
void music_history_clear(music_history_t *h);

/** Clear when the queue was replaced or cleared since the entries were recorded */
void music_history_sync(music_history_t *h, uint32_t queue_epoch);

/** The current entry's path, or NULL */
const char *music_history_current(const music_history_t *h);

/** A track starts that isn't from a history step: forward entries are cut, it becomes current */
void music_history_push(music_history_t *h, const char *path, int hint);

/** A track planned to play next: replaces any forward entries, current stays */
void music_history_plan(music_history_t *h, const char *path, int hint);

/**
 * The queue position of the next entry forward (@p forward) or back, skipping
 * and dropping entries no longer queued; -1 when there is none.  With
 * @p move, that entry becomes current.
 */
int music_history_step(music_history_t *h,
                       bool forward,
                       bool move,
                       music_nav_find_fn find,
                       void *ctx);

/** The current entry wouldn't play: drop it, the one before becomes current */
void music_history_drop_current(music_history_t *h);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_NAV_H */
