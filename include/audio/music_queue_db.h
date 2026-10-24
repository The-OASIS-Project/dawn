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
 *
 * Music Queue DB Persistence - Save/load per-user music queues to SQLite
 */

#ifndef MUSIC_QUEUE_DB_H
#define MUSIC_QUEUE_DB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Full type in audio/music_queue.h */
struct user_music_queue;

/**
 * @brief Initialize the music queue database
 *
 * Opens (or creates) the SQLite database and creates tables if needed.
 * Uses WAL mode and busy timeout for concurrency with music_db scans.
 *
 * @param db_path Path to the database file (e.g., "{data_dir}/music.db")
 * @return 0 on success, non-zero on error
 */
int music_queue_db_init(const char *db_path);

/**
 * @brief Clean up the music queue database
 *
 * Closes the SQLite handle.
 */
void music_queue_db_cleanup(void);

/**
 * @brief Save a user's queue to the database
 *
 * Replaces all existing queue entries for the user with the current queue.
 * Also saves shuffle/repeat_mode state. Must be called with uq->queue_mutex held.
 *
 * @param user_id User ID
 * @param uq User music queue (caller holds queue_mutex)
 * @return 0 on success, non-zero on error
 */
int music_queue_db_save(int user_id, const struct user_music_queue *uq);

/**
 * @brief Save only shuffle/repeat_mode state (not queue contents)
 *
 * Lightweight alternative to music_queue_db_save for state-only changes.
 *
 * @param user_id User ID (skips if <= 0)
 * @param uq User music queue (caller holds queue_mutex)
 * @return 0 on success, non-zero on error
 */
int music_queue_db_save_state(int user_id, const struct user_music_queue *uq);

/**
 * @brief Load a user's queue from the database
 *
 * Populates the queue entries, queue_length, shuffle, and repeat_mode fields.
 * Called during find-or-create when no in-memory queue exists yet.
 *
 * @param user_id User ID
 * @param uq User music queue to populate
 * @return 0 on success (empty queue is still success), non-zero on error
 */
int music_queue_db_load(int user_id, struct user_music_queue *uq);

/**
 * @brief Delete all queue data for a user
 *
 * Removes queue entries and state for the specified user.
 *
 * @param user_id User ID
 * @return 0 on success, non-zero on error
 */
int music_queue_db_delete_user(int user_id);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_QUEUE_DB_H */
