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
 * Music queue: one shared queue per signed-in user (every tab and player of
 * that user sees the same tracks), a private one for anyone else, kept in
 * music.db across restarts.  Each player keeps its own position in it.
 */

#ifndef MUSIC_QUEUE_H
#define MUSIC_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio/music_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Tracks a queue holds */
#define MUSIC_QUEUE_MAX 250

/** Track path length */
#define MUSIC_PATH_MAX MUSIC_DB_PATH_MAX

/** Title/artist/album length (longer ones are cut for display) */
#define MUSIC_STRING_MAX 128

typedef enum {
   MUSIC_REPEAT_NONE = 0,
   MUSIC_REPEAT_ALL = 1,
   MUSIC_REPEAT_ONE = 2,
} music_repeat_mode_t;

typedef struct {
   char path[MUSIC_PATH_MAX];
   char title[MUSIC_STRING_MAX];
   char artist[MUSIC_STRING_MAX];
   char album[MUSIC_STRING_MAX];
   uint32_t duration_sec;
} music_queue_entry_t;

/**
 * A player's place in a queue, attached to it so inserts and removes move it
 * with the tracks.  Guarded by the queue's queue_mutex.
 */
typedef struct music_queue_pos music_queue_pos_t;
struct music_queue_pos {
   music_queue_pos_t *next;
   int index;
};

/**
 * A queue and its play modes.  queue_mutex guards every field but user_id
 * (fixed) and ref_count (the registry's).
 */
typedef struct user_music_queue user_music_queue_t;
struct user_music_queue {
   int user_id; /**< Owner; 0 for a private queue */
   pthread_mutex_t queue_mutex;
   int ref_count;
   uint32_t generation;    /**< Bumped on every change (music_state's queue_rev) */
   uint32_t replace_epoch; /**< Bumped when the queue is replaced or cleared (play history) */
   music_queue_entry_t queue[MUSIC_QUEUE_MAX];
   int queue_length;
   bool shuffle;
   music_repeat_mode_t repeat_mode;
   music_queue_pos_t *positions; /**< Attached players' places */
};

/** What music_queue_add() did with each track */
typedef enum {
   MUSIC_QUEUE_ADDED = 0, /**< Appended */
   MUSIC_QUEUE_DUPLICATE, /**< Already queued */
   MUSIC_QUEUE_NOT_FOUND, /**< No track (empty path) */
   MUSIC_QUEUE_FULL,      /**< Queue at MUSIC_QUEUE_MAX */
} music_queue_outcome_t;

/**
 * Open the queue store (music.db; its tables are created if missing).
 * @return SUCCESS, or FAILURE: queues then work but aren't kept
 */
int music_queue_init(const char *db_path);

/** Free every queue no player holds and close the store (one still held at
 *  exit is left allocated for its last release) */
void music_queue_shutdown(void);

/**
 * A queue for a player: the user's shared one (restored from the store on
 * first use) when @p user_id > 0, else a private one.  Pair with
 * music_queue_release().
 * @return NULL when out of memory
 */
user_music_queue_t *music_queue_acquire(int user_id);

/** Done with a queue from music_queue_acquire() (frees a private one) */
void music_queue_release(user_music_queue_t *uq);

/** Attach a player's place (index 0) so it follows inserts and removes; takes queue_mutex */
void music_queue_attach(user_music_queue_t *uq, music_queue_pos_t *pos);

/** Detach a place before the player goes; takes queue_mutex */
void music_queue_detach(user_music_queue_t *uq, music_queue_pos_t *pos);

/**
 * Where @p uq holds @p path: the copy nearest @p hint (a position it had),
 * or -1.  Caller holds queue_mutex.  Fits music_nav_find_fn's lookup.
 */
int music_queue_find(const user_music_queue_t *uq, const char *path, int hint);

/**
 * Put @p track at @p index (0 ≤ index ≤ queue_length); a full queue drops
 * its last track to make room, unless the track would be that last one.
 * Attached places at or after @p index move down one (not on an append).
 * Caller holds queue_mutex, then calls music_queue_changed().
 * @return SUCCESS, or FAILURE (bad index, or full at the end)
 */
int music_queue_insert(user_music_queue_t *uq, int index, const music_search_result_t *track);

/**
 * Remove the track at @p index; attached places after it move up one, a
 * place on it stays (now the track that took its place).  Caller holds
 * queue_mutex, then calls music_queue_changed().
 */
void music_queue_remove_at(user_music_queue_t *uq, int index);

/**
 * Empty the queue (bumps replace_epoch); every attached place goes to 0.
 * Caller holds queue_mutex, then calls music_queue_changed().
 */
void music_queue_clear(user_music_queue_t *uq);

/**
 * A change to @p uq is done: other players see it (generation), and it's
 * kept (the tracks and modes when @p tracks, else only shuffle/repeat).
 * Caller holds queue_mutex.
 */
void music_queue_changed(user_music_queue_t *uq, bool tracks);

/**
 * Append @p tracks (all of them in place of the queue when @p replace),
 * skipping any already queued; an entry with an empty path is no track.
 * Takes queue_mutex, bumps generation, saves.
 * @param outcomes Optional, one per track
 * @param added_out Optional, tracks appended
 */
void music_queue_add(user_music_queue_t *uq,
                     const music_search_result_t *tracks,
                     int n,
                     bool replace,
                     music_queue_outcome_t *outcomes,
                     int *added_out);

/**
 * Append (or replace) library tracks by path: each is checked
 * (music_path_is_valid) and looked up in the music library before the queue
 * lock; one it can't find is no track.  Then music_queue_add().  Defined in
 * music_queue_apply.c.
 * @param outcomes Optional caller-allocated array of n entries
 * @return SUCCESS, or FAILURE on invalid args / allocation failure
 */
int music_queue_apply_paths(user_music_queue_t *uq,
                            const char *const *paths,
                            int n,
                            bool replace,
                            music_queue_outcome_t *outcomes);

/**
 * Told when a player changed a queue's tracks or modes, so other players'
 * listeners see it.  @p origin identifies the player that changed it (its
 * own listener already knows); runs with no queue lock held.
 */
typedef void (*music_queue_listener_t)(user_music_queue_t *uq, const void *origin);

/** Set the one listener (NULL to clear); the WebUI's tells its tabs */
void music_queue_set_listener(music_queue_listener_t fn);

/** Tell the listener that @p origin changed @p uq; call with queue_mutex released */
void music_queue_announce(user_music_queue_t *uq, const void *origin);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_QUEUE_H */
