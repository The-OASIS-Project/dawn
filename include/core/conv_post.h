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
 * Posts into a conversation from outside any turn: DAWN's own unprompted
 * messages (a job's report, a channel message DAWN sent).  A post never waits
 * for a turn running on the conversation: it is written now when none is, else
 * queued and written by that turn's end, before the next turn starts
 * (conv_turn_lease.h), so its row never lands between a turn's rows.
 */

#ifndef CONV_POST_H
#define CONV_POST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Called once a post's row is written (now or at the holder's end), with no lock held. */
typedef void (*conv_post_written_fn)(int64_t conv_id, int64_t msg_id);

/**
 * @brief Register the post writer with the conversation lease
 *
 * Once at startup, before any turn or post.
 */
void conv_post_init(void);

/**
 * @brief Post @p text into @p conv_id as DAWN's own unprompted assistant
 *        message (shown and replayed, never extracted), and tell its open tabs
 *
 * Never waits.  Safe from any thread holding no lock, including a turn
 * posting into its own conversation (the row is written at that turn's end).
 *
 * @return SUCCESS when written, or queued to be written; FAILURE when it
 *         couldn't be (bad argument, no memory, or the write failed now)
 */
int conv_post_unprompted(int64_t conv_id, int user_id, const char *text);

/**
 * @brief conv_post_unprompted(), calling @p on_written (may be NULL) with the
 *        row's id once it is written
 */
int conv_post_unprompted_ex(int64_t conv_id,
                            int user_id,
                            const char *text,
                            conv_post_written_fn on_written);

#ifdef __cplusplus
}
#endif

#endif /* CONV_POST_H */
